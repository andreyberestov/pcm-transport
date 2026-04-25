#include "pcmtp/backend/AlsaPcmBackend.hpp"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__SSE2__) && (defined(__x86_64__) || defined(__i386__))
#include <emmintrin.h>
#endif

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {

namespace {

void check_alsa(int result, const char* message) {
    if (result < 0) {
        throw std::runtime_error(std::string(message) + ": " + snd_strerror(result));
    }
}

bool hw_format_supported(snd_pcm_t* handle,
                         snd_pcm_hw_params_t* hw_params,
                         snd_pcm_format_t fmt) {
    return snd_pcm_hw_params_test_format(handle, hw_params, fmt) >= 0;
}

std::vector<snd_pcm_format_t> format_candidates_for_bits(std::uint16_t bits_per_sample) {
    std::vector<snd_pcm_format_t> candidates;
    if (bits_per_sample <= 16) {
        candidates.push_back(SND_PCM_FORMAT_S16_LE);
    } else if (bits_per_sample <= 24) {
        candidates.push_back(SND_PCM_FORMAT_S24_LE);
        candidates.push_back(SND_PCM_FORMAT_S24_3LE);
        candidates.push_back(SND_PCM_FORMAT_S32_LE);
    } else {
        candidates.push_back(SND_PCM_FORMAT_S32_LE);
    }
    return candidates;
}

void convert_to_s16_scalar(const PcmSample* samples, std::size_t count, std::uint16_t bits_per_sample, std::int16_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        std::int64_t v = static_cast<std::int64_t>(samples[i]);
        if (bits_per_sample > 16) {
            v >>= (bits_per_sample - 16);
        }
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = static_cast<std::int16_t>(v);
    }
}

bool convert_to_s16(const PcmSample* samples, std::size_t count, std::uint16_t bits_per_sample, bool prefer_simd, std::int16_t* out) {
#if defined(__SSE2__) && (defined(__x86_64__) || defined(__i386__))
    if (prefer_simd && bits_per_sample <= 16) {
        std::size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(samples + i));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(samples + i + 4));
            const __m128i packed = _mm_packs_epi32(a, b);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), packed);
        }
        for (; i < count; ++i) {
            std::int64_t v = static_cast<std::int64_t>(samples[i]);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[i] = static_cast<std::int16_t>(v);
        }
        return true;
    }
#else
    (void)prefer_simd;
#endif
    convert_to_s16_scalar(samples, count, bits_per_sample, out);
    return false;
}

} // namespace

AlsaPcmBackend::~AlsaPcmBackend() {
    close();
}

void AlsaPcmBackend::open(const std::string& device_name, const AudioFormat& format) {
    close();
    format_ = format;
    pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
    simd_conversion_samples_processed_.store(0, std::memory_order_relaxed);

    Logger::instance().info("Opening ALSA device: " + device_name + " format=" + format.to_string());
    check_alsa(snd_pcm_open(&handle_, device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0),
               "snd_pcm_open failed");

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);

    check_alsa(snd_pcm_hw_params_any(handle_, hw_params), "snd_pcm_hw_params_any failed");
    Logger::instance().debug("ALSA hw_params_any ok");
    check_alsa(snd_pcm_hw_params_set_access(handle_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED),
               "snd_pcm_hw_params_set_access failed");
    Logger::instance().debug("ALSA access set: RW_INTERLEAVED");
    check_alsa(snd_pcm_hw_params_set_channels(handle_, hw_params, format.channels),
               "snd_pcm_hw_params_set_channels failed");
    Logger::instance().debug("ALSA channels requested: " + std::to_string(format.channels));

    unsigned sample_rate = format.sample_rate;
    check_alsa(snd_pcm_hw_params_set_rate_near(handle_, hw_params, &sample_rate, nullptr),
               "snd_pcm_hw_params_set_rate_near failed");
    Logger::instance().debug("ALSA rate requested=" + std::to_string(format.sample_rate) + " accepted_near=" + std::to_string(sample_rate));
    if (sample_rate != format.sample_rate) {
        throw std::runtime_error("ALSA device does not accept requested sample rate exactly");
    }

    snd_pcm_uframes_t requested_period = 588;
    snd_pcm_uframes_t requested_buffer = 2352;
    check_alsa(snd_pcm_hw_params_set_period_size_near(handle_, hw_params, &requested_period, nullptr),
               "snd_pcm_hw_params_set_period_size_near failed");
    check_alsa(snd_pcm_hw_params_set_buffer_size_near(handle_, hw_params, &requested_buffer),
               "snd_pcm_hw_params_set_buffer_size_near failed");
    Logger::instance().debug("ALSA period requested_near=" + std::to_string(static_cast<unsigned long long>(requested_period)) +
                             " buffer requested_near=" + std::to_string(static_cast<unsigned long long>(requested_buffer)));

    Logger::instance().debug(std::string("ALSA test_format S16_LE => ") + (hw_format_supported(handle_, hw_params, SND_PCM_FORMAT_S16_LE) ? "supported" : "unsupported"));
    const std::vector<snd_pcm_format_t> candidates = format_candidates_for_bits(format.bits_per_sample);
    bool opened = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        snd_pcm_hw_params_t* trial = nullptr;
        snd_pcm_hw_params_alloca(&trial);
        snd_pcm_hw_params_copy(trial, hw_params);
        const char* candidate_name = snd_pcm_format_name(candidates[i]);
        const bool supported = hw_format_supported(handle_, trial, candidates[i]);
        Logger::instance().debug(std::string("ALSA test_format ") + candidate_name + (supported ? " => supported" : " => unsupported"));
        if (!supported) {
            continue;
        }
        const int set_format_result = snd_pcm_hw_params_set_format(handle_, trial, candidates[i]);
        Logger::instance().debug(std::string("ALSA set_format ") + candidate_name + " => " + std::to_string(set_format_result));
        if (set_format_result < 0) {
            continue;
        }
        const int commit_result = snd_pcm_hw_params(handle_, trial);
        Logger::instance().debug(std::string("ALSA hw_params commit ") + candidate_name + " => " + std::to_string(commit_result));
        if (commit_result < 0) {
            continue;
        }
        opened = true;
        pcm_container_format_ = candidates[i];
        check_alsa(snd_pcm_hw_params_current(handle_, hw_params), "snd_pcm_hw_params_current failed");
        snd_pcm_hw_params_get_period_size(hw_params, &period_frames_, nullptr);
        snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames_);
        snd_pcm_format_t accepted = SND_PCM_FORMAT_UNKNOWN;
        if (snd_pcm_hw_params_get_format(hw_params, &accepted) >= 0) {
            pcm_container_format_ = accepted;
        }
        Logger::instance().info(std::string("ALSA format negotiation: requested bits=") + std::to_string(format.bits_per_sample) +
                                " container=" + candidate_name +
                                " accepted=" + (snd_pcm_format_name(pcm_container_format_) ? snd_pcm_format_name(pcm_container_format_) : "unknown"));
        break;
    }

    if (!opened) {
        const bool s16_supported = hw_format_supported(handle_, hw_params, SND_PCM_FORMAT_S16_LE);
        if (format.bits_per_sample <= 16) {
            throw std::runtime_error("ALSA device does not accept requested 16-bit PCM format");
        }
        if (format.bits_per_sample <= 24) {
            if (s16_supported) {
                throw std::runtime_error("ALSA device does not accept requested 24-bit PCM format (S16_LE is supported in current hw mode)");
            }
            throw std::runtime_error("ALSA device does not accept requested 24-bit PCM format");
        }
        throw std::runtime_error("ALSA device does not accept requested 32-bit PCM format");
    }

    Logger::instance().debug("ALSA PCM container selected: " + std::string(snd_pcm_format_name(pcm_container_format_)));

    snd_pcm_sw_params_t* sw_params = nullptr;
    snd_pcm_sw_params_alloca(&sw_params);
    check_alsa(snd_pcm_sw_params_current(handle_, sw_params), "snd_pcm_sw_params_current failed");
    check_alsa(snd_pcm_sw_params_set_start_threshold(handle_, sw_params, period_frames_),
               "snd_pcm_sw_params_set_start_threshold failed");
    check_alsa(snd_pcm_sw_params_set_avail_min(handle_, sw_params, period_frames_),
               "snd_pcm_sw_params_set_avail_min failed");
    check_alsa(snd_pcm_sw_params(handle_, sw_params), "snd_pcm_sw_params failed");

    check_alsa(snd_pcm_prepare(handle_), "snd_pcm_prepare failed");
    Logger::instance().debug("ALSA opened period=" + std::to_string(static_cast<unsigned long long>(period_frames_)) +
                             " buffer=" + std::to_string(static_cast<unsigned long long>(buffer_frames_)) +
                             " start_threshold=" + std::to_string(static_cast<unsigned long long>(period_frames_)) +
                             " avail_min=" + std::to_string(static_cast<unsigned long long>(period_frames_)) +
                             " model=ALSA PCM ring buffer");
}

void AlsaPcmBackend::set_simd_conversion_enabled(bool enabled) {
    simd_conversion_enabled_ = enabled;
}

std::uint64_t AlsaPcmBackend::simd_conversion_samples_processed() const {
    return simd_conversion_samples_processed_.load(std::memory_order_relaxed);
}

std::size_t AlsaPcmBackend::write_samples(const PcmSample* samples, std::size_t sample_count) {
    if (handle_ == nullptr) {
        throw std::runtime_error("ALSA backend not opened");
    }

    std::size_t written_samples = 0;
    const std::size_t channels = format_.channels;
    while (written_samples < sample_count) {
        const snd_pcm_uframes_t frames_to_write =
            static_cast<snd_pcm_uframes_t>((sample_count - written_samples) / channels);
        if (frames_to_write == 0) {
            break;
        }

        const void* write_ptr = nullptr;
        if (pcm_container_format_ == SND_PCM_FORMAT_S16_LE) {
            temp16_.resize(static_cast<std::size_t>(frames_to_write) * channels);
            const bool used_simd = convert_to_s16(samples + written_samples, temp16_.size(), format_.bits_per_sample, simd_conversion_enabled_, temp16_.data());
            if (used_simd) {
                simd_conversion_samples_processed_.fetch_add(static_cast<std::uint64_t>(temp16_.size()), std::memory_order_relaxed);
            }
            write_ptr = temp16_.data();
        } else if (pcm_container_format_ == SND_PCM_FORMAT_S24_3LE) {
            temp24_.resize(static_cast<std::size_t>(frames_to_write) * channels * 3u);
            const std::int64_t hi = 8388607;
            const std::int64_t lo = -8388608;
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames_to_write) * channels; ++i) {
                std::int64_t v = static_cast<std::int64_t>(samples[written_samples + i]);
                if (format_.bits_per_sample > 24) {
                    v >>= (format_.bits_per_sample - 24);
                }
                if (v > hi) v = hi;
                if (v < lo) v = lo;
                const std::uint32_t u = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
                temp24_[i * 3u + 0u] = static_cast<unsigned char>(u & 0xFFu);
                temp24_[i * 3u + 1u] = static_cast<unsigned char>((u >> 8) & 0xFFu);
                temp24_[i * 3u + 2u] = static_cast<unsigned char>((u >> 16) & 0xFFu);
            }
            write_ptr = temp24_.data();
        } else {
            temp32_.resize(static_cast<std::size_t>(frames_to_write) * channels);
            const bool shift_to_container = (pcm_container_format_ == SND_PCM_FORMAT_S32_LE && format_.bits_per_sample <= 24);
            for (std::size_t i = 0; i < temp32_.size(); ++i) {
                std::int64_t v = static_cast<std::int64_t>(samples[written_samples + i]);
                if (pcm_container_format_ == SND_PCM_FORMAT_S24_LE) {
                    if (format_.bits_per_sample > 24) {
                        v >>= (format_.bits_per_sample - 24);
                    }
                    if (v > 8388607) v = 8388607;
                    if (v < -8388608) v = -8388608;
                } else if (shift_to_container) {
                    v <<= (32 - format_.bits_per_sample);
                }
                if (v > INT32_MAX) v = INT32_MAX;
                if (v < INT32_MIN) v = INT32_MIN;
                temp32_[i] = static_cast<std::int32_t>(v);
            }
            write_ptr = temp32_.data();
        }

        const snd_pcm_sframes_t result = snd_pcm_writei(handle_, write_ptr, frames_to_write);
        if (result == -EPIPE) {
            Logger::instance().debug("ALSA underrun, preparing again");
            snd_pcm_prepare(handle_);
            continue;
        }
        if (result == -ESTRPIPE) {
            Logger::instance().debug("ALSA suspended stream, trying resume");
            while (snd_pcm_resume(handle_) == -EAGAIN) {
            }
            snd_pcm_prepare(handle_);
            continue;
        }
        if (result < 0) {
            throw std::runtime_error(std::string("snd_pcm_writei failed: ") +
                                     snd_strerror(static_cast<int>(result)));
        }

        written_samples += static_cast<std::size_t>(result) * channels;
    }

    return written_samples;
}

void AlsaPcmBackend::drain() {
    if (handle_ != nullptr) {
        Logger::instance().debug("Draining ALSA device");
        snd_pcm_drain(handle_);
    }
}

void AlsaPcmBackend::close() {
    if (handle_ != nullptr) {
        Logger::instance().debug("Closing ALSA device (drop + close)");
        snd_pcm_drop(handle_);
        snd_pcm_close(handle_);
        handle_ = nullptr;
        pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
        temp16_.clear();
        temp32_.clear();
        temp24_.clear();
    }
}

snd_pcm_uframes_t AlsaPcmBackend::period_frames() const {
    return period_frames_;
}

snd_pcm_uframes_t AlsaPcmBackend::buffer_frames() const {
    return buffer_frames_;
}

snd_pcm_format_t AlsaPcmBackend::pcm_container_format() const {
    return pcm_container_format_;
}

} // namespace pcmtp
