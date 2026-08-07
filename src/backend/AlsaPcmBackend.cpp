// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/backend/AlsaPcmBackend.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <limits>
#include <memory>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#include "pcmtp/backend/AlsaBufferPolicy.hpp"
#include "pcmtp/util/Logger.hpp"

namespace pcmtp {

namespace {

void check_alsa(int result, const std::string& message) {
    if (result < 0) {
        throw std::runtime_error(message + ": " + snd_strerror(result));
    }
}

struct PcmHandleDeleter {
    void operator()(snd_pcm_t* handle) const noexcept {
        if (handle != nullptr) {
            snd_pcm_close(handle);
        }
    }
};

using UniquePcmHandle = std::unique_ptr<snd_pcm_t, PcmHandleDeleter>;

void push_unique_format(std::vector<snd_pcm_format_t>& candidates, snd_pcm_format_t fmt) {
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i] == fmt) {
            return;
        }
    }
    candidates.push_back(fmt);
}

std::vector<snd_pcm_format_t> format_candidates_for_bits(std::uint16_t bits_per_sample,
                                                          Alsa24BitContainerPreference preference) {
    std::vector<snd_pcm_format_t> candidates;
    if (bits_per_sample <= 16) {
        candidates.push_back(SND_PCM_FORMAT_S16_LE);
    } else if (bits_per_sample <= 24) {
        switch (preference) {
            case Alsa24BitContainerPreference::PreferS24LE:
                push_unique_format(candidates, SND_PCM_FORMAT_S24_LE);
                break;
            case Alsa24BitContainerPreference::PreferS24_3LE:
                push_unique_format(candidates, SND_PCM_FORMAT_S24_3LE);
                break;
            case Alsa24BitContainerPreference::PreferS32LE:
                push_unique_format(candidates, SND_PCM_FORMAT_S32_LE);
                break;
            case Alsa24BitContainerPreference::Auto:
            default:
                break;
        }
        push_unique_format(candidates, SND_PCM_FORMAT_S24_LE);
        push_unique_format(candidates, SND_PCM_FORMAT_S24_3LE);
        push_unique_format(candidates, SND_PCM_FORMAT_S32_LE);
    } else {
        candidates.push_back(SND_PCM_FORMAT_S32_LE);
    }
    return candidates;
}

const char* format_name_or_unknown(snd_pcm_format_t fmt) {
    const char* name = snd_pcm_format_name(fmt);
    return name != nullptr ? name : "unknown";
}

std::string preference_name(Alsa24BitContainerPreference preference) {
    switch (preference) {
        case Alsa24BitContainerPreference::PreferS24LE: return "Prefer S24_LE";
        case Alsa24BitContainerPreference::PreferS24_3LE: return "Prefer S24_3LE";
        case Alsa24BitContainerPreference::PreferS32LE: return "Prefer S32_LE";
        case Alsa24BitContainerPreference::Auto:
        default:
            return "Auto";
    }
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

void recover_suspended_stream(snd_pcm_t* handle) {
    constexpr unsigned int kMaximumResumeAttempts = 100;
    constexpr auto kResumeRetryDelay = std::chrono::milliseconds(10);

    int resume_result = -EAGAIN;
    for (unsigned int attempt = 0;
         attempt < kMaximumResumeAttempts && resume_result == -EAGAIN;
         ++attempt) {
        resume_result = snd_pcm_resume(handle);
        if (resume_result == -EAGAIN) {
            std::this_thread::sleep_for(kResumeRetryDelay);
        }
    }

    if (resume_result < 0) {
        check_alsa(snd_pcm_prepare(handle),
                   "snd_pcm_prepare failed after suspend");
    }
}

struct PcmCandidateResult {
    UniquePcmHandle handle;
    snd_pcm_format_t container_format = SND_PCM_FORMAT_UNKNOWN;
    unsigned sample_rate = 0;
    snd_pcm_uframes_t period_frames = 0;
    snd_pcm_uframes_t buffer_frames = 0;
    int failure_rank = 0;
    std::string failure_message;

    bool success() const noexcept {
        return handle != nullptr &&
               container_format != SND_PCM_FORMAT_UNKNOWN &&
               sample_rate != 0;
    }
};

PcmCandidateResult try_open_pcm_candidate(
    const std::string& device_name,
    const AudioFormat& format,
    snd_pcm_format_t candidate,
    const AlsaBufferPolicy& target_buffer_policy) {
    PcmCandidateResult result;
    const char* candidate_name = format_name_or_unknown(candidate);
    int failure_rank = 1;

    try {
        snd_pcm_t* raw_handle = nullptr;
        const int open_result = snd_pcm_open(&raw_handle,
                                             device_name.c_str(),
                                             SND_PCM_STREAM_PLAYBACK,
                                             0);
        result.handle.reset(raw_handle);
        check_alsa(open_result, "snd_pcm_open failed");

        snd_pcm_hw_params_t* hw_params = nullptr;
        snd_pcm_hw_params_alloca(&hw_params);

        failure_rank = 2;
        check_alsa(snd_pcm_hw_params_any(result.handle.get(), hw_params),
                   "snd_pcm_hw_params_any failed");

        failure_rank = 3;
        check_alsa(snd_pcm_hw_params_set_access(
                       result.handle.get(),
                       hw_params,
                       SND_PCM_ACCESS_RW_INTERLEAVED),
                   "snd_pcm_hw_params_set_access failed");

        failure_rank = 4;
        check_alsa(snd_pcm_hw_params_set_format(
                       result.handle.get(), hw_params, candidate),
                   std::string("snd_pcm_hw_params_set_format failed for ") +
                       candidate_name);

        failure_rank = 5;
        check_alsa(snd_pcm_hw_params_set_channels(
                       result.handle.get(), hw_params, format.channels),
                   "snd_pcm_hw_params_set_channels failed for native " +
                       std::to_string(format.channels) +
                       "-channel PCM with " + candidate_name);

        failure_rank = 6;
        unsigned accepted_rate = format.sample_rate;
        check_alsa(snd_pcm_hw_params_set_rate_near(
                       result.handle.get(),
                       hw_params,
                       &accepted_rate,
                       nullptr),
                   std::string("snd_pcm_hw_params_set_rate_near failed for ") +
                       candidate_name);
        if (accepted_rate != format.sample_rate) {
            throw std::runtime_error(
                std::string("ALSA device does not accept requested sample rate exactly with ") +
                candidate_name + " (requested " +
                std::to_string(format.sample_rate) + " Hz, nearest " +
                std::to_string(accepted_rate) + " Hz)");
        }

        failure_rank = 7;
        snd_pcm_uframes_t requested_period =
            static_cast<snd_pcm_uframes_t>(
                target_buffer_policy.period_frames);
        check_alsa(snd_pcm_hw_params_set_period_size_near(
                       result.handle.get(),
                       hw_params,
                       &requested_period,
                       nullptr),
                   std::string("snd_pcm_hw_params_set_period_size_near failed for ") +
                       candidate_name);

        const snd_pcm_uframes_t max_frames =
            std::numeric_limits<snd_pcm_uframes_t>::max();
        snd_pcm_uframes_t requested_buffer = requested_period;
        if (requested_period <= max_frames / 4U) {
            requested_buffer = requested_period * 4U;
        } else {
            requested_buffer = max_frames;
        }

        failure_rank = 8;
        check_alsa(snd_pcm_hw_params_set_buffer_size_near(
                       result.handle.get(),
                       hw_params,
                       &requested_buffer),
                   std::string("snd_pcm_hw_params_set_buffer_size_near failed for ") +
                       candidate_name);

        failure_rank = 9;
        check_alsa(snd_pcm_hw_params(result.handle.get(), hw_params),
                   std::string("snd_pcm_hw_params commit failed for ") +
                       candidate_name);

        failure_rank = 10;
        check_alsa(snd_pcm_hw_params_current(result.handle.get(), hw_params),
                   std::string("snd_pcm_hw_params_current failed for ") +
                       candidate_name);

        failure_rank = 11;
        check_alsa(snd_pcm_hw_params_get_period_size(
                       hw_params, &result.period_frames, nullptr),
                   std::string("snd_pcm_hw_params_get_period_size failed for ") +
                       candidate_name);

        failure_rank = 12;
        check_alsa(snd_pcm_hw_params_get_buffer_size(
                       hw_params, &result.buffer_frames),
                   std::string("snd_pcm_hw_params_get_buffer_size failed for ") +
                       candidate_name);

        failure_rank = 13;
        snd_pcm_format_t accepted_format = SND_PCM_FORMAT_UNKNOWN;
        check_alsa(snd_pcm_hw_params_get_format(hw_params, &accepted_format),
                   std::string("snd_pcm_hw_params_get_format failed for ") +
                       candidate_name);
        if (accepted_format != candidate) {
            throw std::runtime_error(
                std::string("ALSA committed an unexpected PCM container for ") +
                candidate_name);
        }

        snd_pcm_sw_params_t* sw_params = nullptr;
        snd_pcm_sw_params_alloca(&sw_params);

        failure_rank = 14;
        check_alsa(snd_pcm_sw_params_current(result.handle.get(), sw_params),
                   std::string("snd_pcm_sw_params_current failed for ") +
                       candidate_name);

        failure_rank = 15;
        check_alsa(snd_pcm_sw_params_set_start_threshold(
                       result.handle.get(),
                       sw_params,
                       result.period_frames),
                   std::string("snd_pcm_sw_params_set_start_threshold failed for ") +
                       candidate_name);

        failure_rank = 16;
        check_alsa(snd_pcm_sw_params_set_avail_min(
                       result.handle.get(),
                       sw_params,
                       result.period_frames),
                   std::string("snd_pcm_sw_params_set_avail_min failed for ") +
                       candidate_name);

        failure_rank = 17;
        check_alsa(snd_pcm_sw_params(result.handle.get(), sw_params),
                   std::string("snd_pcm_sw_params failed for ") +
                       candidate_name);

        failure_rank = 18;
        check_alsa(snd_pcm_prepare(result.handle.get()),
                   std::string("snd_pcm_prepare failed for ") +
                       candidate_name);

        result.container_format = accepted_format;
        result.sample_rate = accepted_rate;
    } catch (const std::runtime_error& ex) {
        result.failure_rank = failure_rank;
        result.failure_message = ex.what();
    }

    return result;
}

} // namespace

AlsaPcmBackend::~AlsaPcmBackend() {
    close();
}

void AlsaPcmBackend::open(const std::string& device_name, const AudioFormat& format) {
    close();

    Logger::instance().info("Opening ALSA device: " + device_name +
                            " format=" + format.to_string());

    const Alsa24BitContainerPreference active_preference =
        format_24bit_preference_;
    const std::vector<snd_pcm_format_t> candidates =
        format_candidates_for_bits(format.bits_per_sample,
                                   active_preference);
    const AlsaBufferPolicy target_buffer_policy =
        alsa_buffer_policy_for_sample_rate(format.sample_rate);

    Logger::instance().debug(
        "ALSA 24-bit container preference: " +
        preference_name(active_preference));

    PcmCandidateResult negotiated;
    int best_failure_rank = -1;
    std::string best_failure_message;

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const snd_pcm_format_t candidate = candidates[i];
        const char* candidate_name = format_name_or_unknown(candidate);
        Logger::instance().debug(
            std::string("ALSA full negotiation attempt: ") +
            candidate_name);

        PcmCandidateResult attempt = try_open_pcm_candidate(
            device_name,
            format,
            candidate,
            target_buffer_policy);
        if (attempt.success()) {
            Logger::instance().info(
                std::string("ALSA format negotiation: requested bits=") +
                std::to_string(format.bits_per_sample) +
                " preference=" + preference_name(active_preference) +
                " accepted=" +
                format_name_or_unknown(attempt.container_format));
            negotiated = std::move(attempt);
            break;
        }

        Logger::instance().debug(
            std::string("ALSA negotiation rejected ") +
            candidate_name + ": " + attempt.failure_message);
        if (attempt.failure_rank <= 3) {
            best_failure_rank = attempt.failure_rank;
            best_failure_message = attempt.failure_message;
            break;
        }
        if (attempt.failure_rank > best_failure_rank) {
            best_failure_rank = attempt.failure_rank;
            best_failure_message = attempt.failure_message;
        }
    }

    if (!negotiated.success()) {
        if (!best_failure_message.empty()) {
            throw std::runtime_error(best_failure_message);
        }
        throw std::runtime_error(
            "ALSA device does not accept any permitted PCM container");
    }

    handle_ = negotiated.handle.release();
    format_ = format;
    pcm_container_format_ = negotiated.container_format;
    active_format_24bit_preference_ = active_preference;
    device_name_ = device_name;
    accepted_sample_rate_ = negotiated.sample_rate;
    period_frames_ = negotiated.period_frames;
    buffer_frames_ = negotiated.buffer_frames;

    Logger::instance().debug(
        "ALSA target period/buffer=" +
        std::to_string(static_cast<unsigned long long>(
            target_buffer_policy.period_frames)) +
        "/" +
        std::to_string(static_cast<unsigned long long>(
            target_buffer_policy.buffer_frames)) +
        " actual=" +
        std::to_string(static_cast<unsigned long long>(period_frames_)) +
        "/" +
        std::to_string(static_cast<unsigned long long>(buffer_frames_)));
    Logger::instance().debug(
        "ALSA opened period=" +
        std::to_string(static_cast<unsigned long long>(period_frames_)) +
        " buffer=" +
        std::to_string(static_cast<unsigned long long>(buffer_frames_)) +
        " start_threshold=" +
        std::to_string(static_cast<unsigned long long>(period_frames_)) +
        " avail_min=" +
        std::to_string(static_cast<unsigned long long>(period_frames_)) +
        " model=ALSA PCM ring buffer");
}

std::size_t AlsaPcmBackend::write_samples(const PcmSample* samples, std::size_t sample_count) {
    if (handle_ == nullptr) {
        throw std::runtime_error("ALSA backend not opened");
    }

    std::size_t written_samples = 0;
    const std::size_t channels = format_.channels;
    if (channels == 0 || sample_count % channels != 0) {
        throw std::runtime_error("ALSA write received an incomplete PCM frame");
    }
    while (written_samples < sample_count) {
        const snd_pcm_uframes_t frames_to_write =
            static_cast<snd_pcm_uframes_t>((sample_count - written_samples) / channels);
        if (frames_to_write == 0) {
            break;
        }

        const void* write_ptr = nullptr;
        if (pcm_container_format_ == SND_PCM_FORMAT_S16_LE) {
            scratch_s16_.resize(static_cast<std::size_t>(frames_to_write) * channels);
            convert_to_s16_scalar(samples + written_samples,
                                  scratch_s16_.size(),
                                  format_.bits_per_sample,
                                  scratch_s16_.data());
            write_ptr = scratch_s16_.data();
        } else if (pcm_container_format_ == SND_PCM_FORMAT_S24_3LE) {
            scratch_s24_.resize(
                static_cast<std::size_t>(frames_to_write) * channels * 3u);
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
                scratch_s24_[i * 3u + 0u] = static_cast<unsigned char>(u & 0xFFu);
                scratch_s24_[i * 3u + 1u] = static_cast<unsigned char>((u >> 8) & 0xFFu);
                scratch_s24_[i * 3u + 2u] = static_cast<unsigned char>((u >> 16) & 0xFFu);
            }
            write_ptr = scratch_s24_.data();
        } else {
            scratch_s32_.resize(static_cast<std::size_t>(frames_to_write) * channels);
            const bool shift_to_container = (pcm_container_format_ == SND_PCM_FORMAT_S32_LE && format_.bits_per_sample <= 24);
            for (std::size_t i = 0; i < scratch_s32_.size(); ++i) {
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
                scratch_s32_[i] = static_cast<std::int32_t>(v);
            }
            write_ptr = scratch_s32_.data();
        }

        const snd_pcm_sframes_t result = snd_pcm_writei(handle_, write_ptr, frames_to_write);
        if (result == -EPIPE) {
            Logger::instance().debug("ALSA underrun, preparing again");
            check_alsa(snd_pcm_prepare(handle_),
                       "snd_pcm_prepare failed after underrun");
            continue;
        }
        if (result == -ESTRPIPE) {
            Logger::instance().debug("ALSA suspended stream, trying resume");
            recover_suspended_stream(handle_);
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
        accepted_sample_rate_ = 0;
    }
}

void AlsaPcmBackend::set_24bit_container_preference(Alsa24BitContainerPreference preference) {
    format_24bit_preference_ = preference;
}

std::string AlsaPcmBackend::active_output_report() const {
    if (pcm_container_format_ == SND_PCM_FORMAT_UNKNOWN || accepted_sample_rate_ == 0) {
        return std::string();
    }
    std::ostringstream ss;
    ss << "Device: " << (device_name_.empty() ? std::string("unknown") : device_name_) << '\n';
    ss << "Source/requested: " << format_.bits_per_sample << "-bit / "
       << format_.sample_rate << " Hz / " << static_cast<unsigned>(format_.channels) << " ch" << '\n';
    ss << "ALSA container: " << format_name_or_unknown(pcm_container_format_)
       << " (24-bit preference: "
       << preference_name(active_format_24bit_preference_) << ")" << '\n';
    ss << "ALSA rate: " << accepted_sample_rate_
       << (accepted_sample_rate_ == format_.sample_rate ? " Hz exact" : " Hz near") << '\n';
    const AlsaBufferPolicy target_policy = alsa_buffer_policy_for_sample_rate(format_.sample_rate);
    ss << "Target period/buffer: " << static_cast<unsigned long long>(target_policy.period_frames)
       << "/" << static_cast<unsigned long long>(target_policy.buffer_frames) << '\n';
    ss << "Actual ALSA period/buffer: " << static_cast<unsigned long long>(period_frames_)
       << "/" << static_cast<unsigned long long>(buffer_frames_);
    return ss.str();
}

AlsaProbeMatrix AlsaPcmBackend::probe_device_format_matrix(const std::string& device_name) {
    const std::vector<unsigned> rates = {
        44100, 48000, 88200, 96000, 176400, 192000,
        352800, 384000, 705600, 768000, 1411200, 1536000
    };
    const std::vector<snd_pcm_format_t> formats = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S32_LE
    };

    AlsaProbeMatrix matrix;
    matrix.device_name = device_name.empty() ? std::string("default") : device_name;
    matrix.sample_rates = rates;
    for (std::size_t f = 0; f < formats.size(); ++f) {
        matrix.format_names.push_back(format_name_or_unknown(formats[f]));
    }

    for (std::size_t f = 0; f < formats.size(); ++f) {
        const snd_pcm_format_t fmt = formats[f];
        for (std::size_t r = 0; r < rates.size(); ++r) {
            bool ok = false;
            snd_pcm_t* handle = nullptr;
            const int open_result = snd_pcm_open(&handle, matrix.device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
            if (open_result >= 0 && handle != nullptr) {
                snd_pcm_hw_params_t* hw_params = nullptr;
                snd_pcm_hw_params_alloca(&hw_params);
                if (snd_pcm_hw_params_any(handle, hw_params) >= 0 &&
                    snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
                    snd_pcm_hw_params_set_format(handle, hw_params, fmt) >= 0 &&
                    snd_pcm_hw_params_set_channels(handle, hw_params, 2) >= 0) {
                    unsigned rate = rates[r];
                    if (snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, nullptr) >= 0 &&
                        rate == rates[r] &&
                        snd_pcm_hw_params(handle, hw_params) >= 0) {
                        ok = true;
                    }
                }
                snd_pcm_close(handle);
            }
            AlsaProbeCell cell;
            cell.format_name = format_name_or_unknown(fmt);
            cell.sample_rate = rates[r];
            cell.supported = ok;
            matrix.cells.push_back(cell);
        }
    }

    return matrix;
}

} // namespace pcmtp
