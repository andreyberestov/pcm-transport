// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/backend/AlsaPcmBackend.hpp"

#include <algorithm>
#include <atomic>
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

using DiagnosticClock = std::chrono::steady_clock;

std::atomic<std::uint64_t> next_alsa_session_id{1};

std::int64_t elapsed_us(DiagnosticClock::time_point since,
                        DiagnosticClock::time_point until) {
    return std::chrono::duration_cast<std::chrono::microseconds>(until - since).count();
}

std::string pcm_state_and_delay(snd_pcm_t* handle) {
    const snd_pcm_state_t state = snd_pcm_state(handle);
    snd_pcm_sframes_t delay_frames = 0;
    const int delay_result = snd_pcm_delay(handle, &delay_frames);

    std::ostringstream out;
    out << "state=" << snd_pcm_state_name(state);
    if (delay_result >= 0) {
        out << " delay_frames=" << static_cast<long long>(delay_frames);
    } else {
        out << " delay_error=" << delay_result
            << " (" << snd_strerror(delay_result) << ')';
    }
    return out.str();
}

// Diagnostic readback only. Failure must not alter a usable PCM configuration.
std::string pcm_sw_params_readback(snd_pcm_t* handle) {
    snd_pcm_sw_params_t* params = nullptr;
    snd_pcm_sw_params_alloca(&params);
    const int current_result = snd_pcm_sw_params_current(handle, params);
    if (current_result < 0) {
        return std::string("sw_params_error=") + snd_strerror(current_result);
    }
    snd_pcm_uframes_t start_threshold = 0;
    snd_pcm_uframes_t avail_min = 0;
    const int start_result = snd_pcm_sw_params_get_start_threshold(params, &start_threshold);
    const int avail_result = snd_pcm_sw_params_get_avail_min(params, &avail_min);
    if (start_result < 0 || avail_result < 0) {
        return std::string("sw_params_readback_error=") +
               snd_strerror(start_result < 0 ? start_result : avail_result);
    }
    return "start_threshold=" +
           std::to_string(static_cast<unsigned long long>(start_threshold)) +
           " avail_min=" +
           std::to_string(static_cast<unsigned long long>(avail_min));
}

class AlsaCallError : public std::runtime_error {
public:
    AlsaCallError(int result, const std::string& message)
        : std::runtime_error(message + ": " + snd_strerror(result)),
          result_(result) {}

    int result() const noexcept { return result_; }

private:
    int result_ = 0;
};

void check_alsa(int result, const std::string& message) {
    if (result < 0) {
        throw AlsaCallError(result, message);
    }
}

bool is_format_combination_error(int failure_rank, int alsa_result) noexcept {
    if (failure_rank != 4 && failure_rank != 5 &&
        failure_rank != 6 && failure_rank != 9) {
        return false;
    }
    if (alsa_result == -EINVAL) {
        return true;
    }
#ifdef ENOTSUP
    if (alsa_result == -ENOTSUP) {
        return true;
    }
#endif
#ifdef EOPNOTSUPP
    if (alsa_result == -EOPNOTSUPP) {
        return true;
    }
#endif
    return false;
}

struct PcmHandleDeleter {
    void operator()(snd_pcm_t* handle) const noexcept {
        if (handle != nullptr) {
            snd_pcm_close(handle);
        }
    }
};

using UniquePcmHandle = std::unique_ptr<snd_pcm_t, PcmHandleDeleter>;

std::vector<snd_pcm_format_t> format_candidates_for_output(
    PcmOutputFormat output_format,
    Alsa24BitContainerMode mode) {
    std::vector<snd_pcm_format_t> candidates;
    switch (output_format) {
    case PcmOutputFormat::S16:
        candidates.push_back(SND_PCM_FORMAT_S16_LE);
        break;
    case PcmOutputFormat::S24:
        switch (mode) {
        case Alsa24BitContainerMode::S24LE:
            candidates.push_back(SND_PCM_FORMAT_S24_LE);
            break;
        case Alsa24BitContainerMode::S24_3LE:
            candidates.push_back(SND_PCM_FORMAT_S24_3LE);
            break;
        case Alsa24BitContainerMode::S32LE:
            candidates.push_back(SND_PCM_FORMAT_S32_LE);
            break;
        case Alsa24BitContainerMode::Auto:
        default:
            candidates.push_back(SND_PCM_FORMAT_S24_LE);
            candidates.push_back(SND_PCM_FORMAT_S24_3LE);
            candidates.push_back(SND_PCM_FORMAT_S32_LE);
            break;
        }
        break;
    case PcmOutputFormat::S32:
        candidates.push_back(SND_PCM_FORMAT_S32_LE);
        break;
    case PcmOutputFormat::Float32:
        candidates.push_back(SND_PCM_FORMAT_FLOAT);
        break;
    case PcmOutputFormat::Float64:
        candidates.push_back(SND_PCM_FORMAT_FLOAT64);
        break;
    }
    return candidates;
}

const char* format_name_or_unknown(snd_pcm_format_t fmt) {
    const char* name = snd_pcm_format_name(fmt);
    return name != nullptr ? name : "unknown";
}

std::string container_mode_name(Alsa24BitContainerMode mode) {
    switch (mode) {
        case Alsa24BitContainerMode::S24LE: return "S24_LE";
        case Alsa24BitContainerMode::S24_3LE: return "S24_3LE";
        case Alsa24BitContainerMode::S32LE: return "S32_LE";
        case Alsa24BitContainerMode::Auto:
        default:
            return "Auto";
    }
}

void pack_s16_exact(const PcmSample* samples,
                    std::size_t count,
                    std::int16_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t value = static_cast<std::int64_t>(samples[i]);
        if (value > 32767 || value < -32768) {
            throw std::runtime_error(
                "ALSA S16 packing received an out-of-range logical PCM sample");
        }
        out[i] = static_cast<std::int16_t>(value);
    }
}

// A successful resume preserves queued PCM; prepare discards it.
enum class SuspendRecoveryResult { Resumed, Prepared };

SuspendRecoveryResult recover_suspended_stream(snd_pcm_t* handle) {
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
        return SuspendRecoveryResult::Prepared;
    }
    return SuspendRecoveryResult::Resumed;
}

struct PcmCandidateResult {
    UniquePcmHandle handle;
    snd_pcm_format_t container_format = SND_PCM_FORMAT_UNKNOWN;
    unsigned sample_rate = 0;
    snd_pcm_uframes_t period_frames = 0;
    snd_pcm_uframes_t buffer_frames = 0;
    int significant_bits = 0;
    int failure_rank = 0;
    bool format_unsupported = false;
    std::string failure_message;
    std::int64_t open_call_us = -1;
    std::int64_t candidate_total_us = 0;

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
    const AlsaBufferPolicy& target_buffer_policy,
    bool measure_timing) {
    PcmCandidateResult result;
    const char* candidate_name = format_name_or_unknown(candidate);
    int failure_rank = 1;
    const auto candidate_started = measure_timing
        ? DiagnosticClock::now() : DiagnosticClock::time_point{};

    try {
        snd_pcm_t* raw_handle = nullptr;
        const auto call_started = measure_timing
            ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        const int open_result = snd_pcm_open(&raw_handle,
                                             device_name.c_str(),
                                             SND_PCM_STREAM_PLAYBACK,
                                             0);
        if (measure_timing) {
            result.open_call_us = elapsed_us(call_started, DiagnosticClock::now());
        }
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
            throw AudioFormatUnsupportedError(
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

        const int significant_bits = snd_pcm_hw_params_get_sbits(hw_params);
        if (significant_bits > 0) {
            result.significant_bits = significant_bits;
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
    } catch (const AudioFormatUnsupportedError& ex) {
        result.failure_rank = failure_rank;
        result.format_unsupported = true;
        result.failure_message = ex.what();
    } catch (const AlsaCallError& ex) {
        result.failure_rank = failure_rank;
        result.format_unsupported =
            is_format_combination_error(failure_rank, ex.result());
        result.failure_message = ex.what();
    } catch (const std::runtime_error& ex) {
        result.failure_rank = failure_rank;
        result.format_unsupported = false;
        result.failure_message = ex.what();
    }

    if (measure_timing) {
        result.candidate_total_us = elapsed_us(candidate_started, DiagnosticClock::now());
    }
    return result;
}

bool probe_pcm_format_rate(const std::string& device_name,
                           snd_pcm_format_t format,
                           unsigned requested_rate) {
    snd_pcm_t* handle = nullptr;
    const int open_result = snd_pcm_open(
        &handle,
        device_name.c_str(),
        SND_PCM_STREAM_PLAYBACK,
        SND_PCM_NONBLOCK);
    if (open_result < 0 || handle == nullptr) {
        return false;
    }

    bool supported = false;
    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    if (snd_pcm_hw_params_any(handle, hw_params) >= 0 &&
        snd_pcm_hw_params_set_access(
            handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
        snd_pcm_hw_params_set_format(handle, hw_params, format) >= 0 &&
        snd_pcm_hw_params_set_channels(handle, hw_params, 2) >= 0) {
        unsigned accepted_rate = requested_rate;
        if (snd_pcm_hw_params_set_rate_near(
                handle, hw_params, &accepted_rate, nullptr) >= 0 &&
            accepted_rate == requested_rate &&
            snd_pcm_hw_params(handle, hw_params) >= 0) {
            supported = true;
        }
    }

    snd_pcm_close(handle);
    return supported;
}

} // namespace

AlsaPcmBackend::~AlsaPcmBackend() {
    close();
}

void AlsaPcmBackend::open(const std::string& device_name,
                         const AudioFormat& format,
                         PcmOutputFormat output_format) {
    close();

    diagnostic_enabled_ = Logger::instance().debug_enabled();
    if (diagnostic_enabled_) {
        diagnostic_session_id_ = next_alsa_session_id.fetch_add(1, std::memory_order_relaxed);
        diagnostic_open_started_at_ = DiagnosticClock::now();
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " begin device=" + device_name);
    }

    Logger::instance().info(
        "Opening ALSA device: " + device_name +
        " format=" + std::string(pcm_output_format_name(output_format)) +
        " / " + std::to_string(format.sample_rate) + " Hz / " +
        std::to_string(format.channels) + " ch");

    const Alsa24BitContainerMode active_mode = format_24bit_mode_;
    const std::vector<snd_pcm_format_t> candidates =
        format_candidates_for_output(output_format, active_mode);
    const AlsaBufferPolicy target_buffer_policy =
        alsa_buffer_policy_for_sample_rate(format.sample_rate);

    Logger::instance().debug(
        "ALSA 24-bit container mode: " + container_mode_name(active_mode));

    PcmCandidateResult negotiated;
    std::string last_unsupported_message;
    for (const snd_pcm_format_t candidate : candidates) {
        const char* candidate_name = format_name_or_unknown(candidate);
        Logger::instance().debug(
            std::string("ALSA full negotiation attempt: ") + candidate_name);
        PcmCandidateResult attempt = try_open_pcm_candidate(
            device_name, format, candidate, target_buffer_policy, diagnostic_enabled_);
        if (diagnostic_enabled_) {
            Logger::instance().debug(
                "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                " candidate=" + candidate_name +
                " snd_pcm_open_us=" + std::to_string(attempt.open_call_us) +
                " candidate_total_us=" + std::to_string(attempt.candidate_total_us) +
                " result=" + (attempt.success() ? "accepted" : "rejected"));
        }
        if (attempt.success()) {
            Logger::instance().info(
                std::string("ALSA format negotiation: requested=") +
                pcm_output_format_name(output_format) +
                " container_mode=" + container_mode_name(active_mode) +
                " accepted=" + format_name_or_unknown(attempt.container_format));
            negotiated = std::move(attempt);
            break;
        }
        Logger::instance().debug(
            std::string("ALSA negotiation rejected ") + candidate_name +
            ": " + attempt.failure_message);
        if (!attempt.format_unsupported) {
            throw std::runtime_error(attempt.failure_message.empty()
                ? std::string("ALSA output setup failed")
                : attempt.failure_message);
        }
        last_unsupported_message = attempt.failure_message;
    }

    if (!negotiated.success()) {
        if (output_format == PcmOutputFormat::Float32) {
            throw AudioFormatUnsupportedError(
                "ALSA device does not support Float32 PCM");
        }
        if (output_format == PcmOutputFormat::Float64) {
            throw AudioFormatUnsupportedError(
                "ALSA device does not support Float64 PCM");
        }
        if (output_format == PcmOutputFormat::S32) {
            throw AudioFormatUnsupportedError(
                "ALSA device does not support S32_LE required for 32-bit precision");
        }
        if (output_format == PcmOutputFormat::S24) {
            if (active_mode != Alsa24BitContainerMode::Auto) {
                throw AudioFormatUnsupportedError(
                    std::string("ALSA device does not support selected 24-bit container ") +
                    container_mode_name(active_mode));
            }
            throw AudioFormatUnsupportedError(
                "ALSA device does not support any permitted 24-bit container "
                "(S24_LE, S24_3LE, S32_LE)");
        }
        throw AudioFormatUnsupportedError(
            last_unsupported_message.empty()
                ? std::string("ALSA device does not support S16_LE")
                : last_unsupported_message);
    }

    handle_ = negotiated.handle.release();
    format_ = format;
    output_format_ = output_format;
    pcm_container_format_ = negotiated.container_format;
    device_name_ = device_name;
    accepted_sample_rate_ = negotiated.sample_rate;
    active_significant_bits_ = negotiated.significant_bits;
    period_frames_ = negotiated.period_frames;
    buffer_frames_ = negotiated.buffer_frames;
    // The pre-roll belongs to this PCM handle, not to the decoder or track position.
    start_silence_total_frames_ =
        (static_cast<std::uint64_t>(accepted_sample_rate_) * start_silence_ms_) / 1000u;
    start_silence_remaining_frames_ = start_silence_total_frames_;
    start_silence_started_ = false;
    start_silence_recoveries_ = 0;
    scratch_start_silence_.clear();

    // Capability is needed for playback control even when logging is disabled.
    snd_pcm_hw_params_t* current_hw = nullptr;
    snd_pcm_hw_params_alloca(&current_hw);
    const int hw_result = snd_pcm_hw_params_current(handle_, current_hw);
    hw_pause_supported_ = hw_result >= 0 &&
                          snd_pcm_hw_params_can_pause(current_hw) > 0;
    hw_pause_active_ = false;
    if (diagnostic_enabled_ && hw_result < 0) {
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " hw_pause_capability_query_failed=" + snd_strerror(hw_result));
    }

    if (diagnostic_enabled_) {
        diagnostic_open_ready_at_ = DiagnosticClock::now();
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " ready total_open_us=" +
            std::to_string(elapsed_us(diagnostic_open_started_at_, diagnostic_open_ready_at_)) +
            " format=" + format_name_or_unknown(pcm_container_format_) +
            " rate=" + std::to_string(accepted_sample_rate_) +
            " channels=" + std::to_string(format_.channels) +
            " hw_pause=" + (hw_pause_supported_ ? "yes" : "no") +
            " start_silence_ms=" + std::to_string(start_silence_ms_) +
            " start_silence_frames=" + std::to_string(start_silence_total_frames_) +
            " period_frames=" +
            std::to_string(static_cast<unsigned long long>(period_frames_)) +
            " buffer_frames=" +
            std::to_string(static_cast<unsigned long long>(buffer_frames_)) +
            " " + pcm_sw_params_readback(handle_) +
            " " + pcm_state_and_delay(handle_));
    }

    Logger::instance().debug(
        "ALSA target period/buffer=" +
        std::to_string(static_cast<unsigned long long>(target_buffer_policy.period_frames)) +
        "/" +
        std::to_string(static_cast<unsigned long long>(target_buffer_policy.buffer_frames)) +
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

bool AlsaPcmBackend::write_start_silence_chunk() {
    if (software_resume_check_pending_) {
        software_resume_check_pending_ = false;
        if (handle_ != nullptr && snd_pcm_state(handle_) == SND_PCM_STATE_XRUN) {
            check_alsa(snd_pcm_prepare(handle_),
                       "snd_pcm_prepare failed after software pause underrun");
            start_silence_remaining_frames_ = start_silence_total_frames_;
            start_silence_started_ = false;
            start_silence_recoveries_ = 0;
            if (diagnostic_enabled_) {
                Logger::instance().debug(
                    "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                    " resume_fallback_xrun_recovered silence_frames=" +
                    std::to_string(start_silence_remaining_frames_) +
                    " " + pcm_state_and_delay(handle_));
            }
        }
    }
    if (start_silence_remaining_frames_ == 0) {
        return false;
    }
    if (handle_ == nullptr) {
        throw std::runtime_error("ALSA startup silence requires an open backend");
    }
    // Keep each blocking write small: PlaybackEngine checks Stop between chunks.
    const snd_pcm_uframes_t chunk_frames = static_cast<snd_pcm_uframes_t>(
        std::min<std::uint64_t>(start_silence_remaining_frames_,
            std::min<std::uint64_t>(2048u,
                std::max<std::uint64_t>(1u, buffer_frames_ / 2u))));
    if (scratch_start_silence_.empty()) {
        // Prepare a bounded, reusable block once per ALSA session. Short
        // final writes reuse its prefix without refilling the whole buffer.
        const snd_pcm_uframes_t capacity_frames = static_cast<snd_pcm_uframes_t>(
            std::min<std::uint64_t>(2048u,
                std::max<std::uint64_t>(1u, buffer_frames_ / 2u)));
        const snd_pcm_sframes_t bytes = snd_pcm_frames_to_bytes(handle_, capacity_frames);
        if (bytes <= 0 || format_.channels == 0 ||
            capacity_frames > std::numeric_limits<unsigned>::max() / format_.channels) {
            throw std::runtime_error("ALSA startup silence has invalid PCM frame geometry");
        }
        scratch_start_silence_.resize(static_cast<std::size_t>(bytes));
        check_alsa(snd_pcm_format_set_silence(
                       pcm_container_format_, scratch_start_silence_.data(),
                       static_cast<unsigned>(capacity_frames * format_.channels)),
                   "ALSA startup silence format is unsupported");
    }

    if (!start_silence_started_) {
        start_silence_started_ = true;
        if (diagnostic_enabled_) {
            Logger::instance().debug(
                "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                " start_silence_begin frames=" + std::to_string(start_silence_total_frames_) +
                " " + pcm_state_and_delay(handle_));
        }
    }
    const snd_pcm_sframes_t result = snd_pcm_writei(
        handle_, scratch_start_silence_.data(), chunk_frames);
    if (result == -EPIPE) {
        if (diagnostic_enabled_) {
            ++diagnostic_epipe_count_;
            if (diagnostic_epipe_count_ <= 8) {
                Logger::instance().debug(
                    "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                    " EPIPE during=start_silence count=" +
                    std::to_string(diagnostic_epipe_count_) + " " + pcm_state_and_delay(handle_));
            }
        } else {
            Logger::instance().debug("ALSA underrun, preparing again");
        }
        // snd_pcm_prepare() discards queued silence. Prime the entire
        // interval again before admitting the first musical sample.
        if (++start_silence_recoveries_ > 16u) {
            throw std::runtime_error("ALSA startup silence exceeded XRUN recovery limit");
        }
        check_alsa(snd_pcm_prepare(handle_),
                   "snd_pcm_prepare failed after startup silence underrun");
        start_silence_remaining_frames_ = start_silence_total_frames_;
        start_silence_started_ = false;
        if (diagnostic_enabled_ && diagnostic_epipe_count_ <= 8) {
            Logger::instance().debug(
                "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                " EPIPE recovered during=start_silence restart_frames=" +
                std::to_string(start_silence_remaining_frames_) + " " +
                pcm_state_and_delay(handle_));
        }
        return true;
    }
    if (result == -ESTRPIPE) {
        if (diagnostic_enabled_) {
            ++diagnostic_suspend_count_;
            if (diagnostic_suspend_count_ <= 8) {
                Logger::instance().debug(
                    "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                    " ESTRPIPE during=start_silence count=" +
                    std::to_string(diagnostic_suspend_count_) + " " + pcm_state_and_delay(handle_));
            }
        } else {
            Logger::instance().debug("ALSA suspended stream, trying resume");
        }
        if (++start_silence_recoveries_ > 16u) {
            throw std::runtime_error("ALSA startup silence exceeded recovery limit");
        }
        if (recover_suspended_stream(handle_) == SuspendRecoveryResult::Prepared) {
            // prepare discards previously queued silence; start the full interval again.
            start_silence_remaining_frames_ = start_silence_total_frames_;
            start_silence_started_ = false;
            if (diagnostic_enabled_ && diagnostic_suspend_count_ <= 8) {
                Logger::instance().debug(
                    "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                    " ESTRPIPE prepared during=start_silence restart_frames=" +
                    std::to_string(start_silence_remaining_frames_) + " " +
                    pcm_state_and_delay(handle_));
            }
        }
        return true;
    }
    if (result < 0) {
        throw std::runtime_error(std::string("ALSA startup silence write failed: ") +
                                 snd_strerror(static_cast<int>(result)));
    }
    if (result == 0) {
        throw std::runtime_error("ALSA startup silence write made no progress");
    }
    start_silence_remaining_frames_ -= static_cast<std::uint64_t>(result);
    if (start_silence_remaining_frames_ == 0 && diagnostic_enabled_) {
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " start_silence_complete frames=" + std::to_string(start_silence_total_frames_) +
            " " + pcm_state_and_delay(handle_));
    }
    return start_silence_remaining_frames_ != 0;
}

bool AlsaPcmBackend::pause_device() {
    if (handle_ == nullptr || !hw_pause_supported_) {
        if (diagnostic_enabled_) log_transport_state("pause_fallback_unsupported");
        return false;
    }
    const snd_pcm_state_t state = snd_pcm_state(handle_);
    if (state != SND_PCM_STATE_RUNNING) {
        if (diagnostic_enabled_) log_transport_state("pause_fallback_not_running");
        return false;
    }
    const int result = snd_pcm_pause(handle_, 1);
    if (diagnostic_enabled_) {
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " pause_device result=" + std::to_string(result) +
            " mode=" + (result == 0 ? "hardware" : "software_fallback") +
            " " + pcm_state_and_delay(handle_));
    }
    if (result < 0) {
        Logger::instance().debug(
            std::string("ALSA hardware pause failed, using software fallback: ") +
            snd_strerror(result));
        return false;
    }
    hw_pause_active_ = true;
    return true;
}

void AlsaPcmBackend::resume_device() {
    if (!hw_pause_active_) return;
    const int result = snd_pcm_pause(handle_, 0);
    if (diagnostic_enabled_) {
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " resume_device result=" + std::to_string(result) +
            " " + pcm_state_and_delay(handle_));
    }
    if (result < 0) {
        // Do not discard the queued musical frames with drop/prepare.
        throw std::runtime_error(std::string("ALSA hardware resume failed: ") +
                                 snd_strerror(result));
    }
    hw_pause_active_ = false;
}

void AlsaPcmBackend::resume_software_pause() {
    if (handle_ == nullptr) return;
    if (diagnostic_enabled_) log_transport_state("resume_software_fallback");
    // Check for XRUN immediately before the next musical block. At 0 ms,
    // keep the original writei/EPIPE recovery path unchanged.
    software_resume_check_pending_ = start_silence_total_frames_ != 0;
}

void AlsaPcmBackend::log_transport_state(const char* event) {
    if (diagnostic_enabled_ && handle_ != nullptr) {
        Logger::instance().debug(
            "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
            " transport=" + event + " hw_pause=" +
            (hw_pause_supported_ ? "yes" : "no") +
            " " + pcm_state_and_delay(handle_));
    }
}

std::size_t AlsaPcmBackend::write_samples(const PcmBlock& block,
                                         std::size_t sample_offset,
                                         std::size_t sample_count) {
    if (handle_ == nullptr) {
        throw std::runtime_error("ALSA backend not opened");
    }
    if (sample_offset > block.size() || sample_count > block.size() - sample_offset) {
        throw std::runtime_error("ALSA write received an invalid PCM block range");
    }

    const std::size_t channels = format_.channels;
    if (channels == 0 || sample_count % channels != 0) {
        throw std::runtime_error("ALSA write received an incomplete PCM frame");
    }

    if (output_format_ == PcmOutputFormat::Float32 &&
        block.storage_format() != PcmStorageFormat::Float32) {
        throw std::runtime_error("ALSA Float32 output received non-Float32 PCM storage");
    }
    if (output_format_ == PcmOutputFormat::Float64 &&
        block.storage_format() != PcmStorageFormat::Float64) {
        throw std::runtime_error("ALSA Float64 output received non-Float64 PCM storage");
    }
    if (!pcm_output_is_floating(output_format_) &&
        block.storage_format() != PcmStorageFormat::NativeInt32) {
        throw std::runtime_error("ALSA integer output received non-integer PCM storage");
    }

    std::size_t written_samples = 0;
    while (written_samples < sample_count) {
        const snd_pcm_uframes_t frames_to_write = static_cast<snd_pcm_uframes_t>(
            (sample_count - written_samples) / channels);
        if (frames_to_write == 0) break;
        const std::size_t chunk_samples =
            static_cast<std::size_t>(frames_to_write) * channels;
        const std::size_t base = sample_offset + written_samples;

        const void* write_ptr = nullptr;
        if (pcm_container_format_ == SND_PCM_FORMAT_FLOAT) {
            write_ptr = block.float32().data() + base;
        } else if (pcm_container_format_ == SND_PCM_FORMAT_FLOAT64) {
            write_ptr = block.float64().data() + base;
        } else {
            const PcmSample* samples = block.native().data();
            if (pcm_container_format_ == SND_PCM_FORMAT_S16_LE) {
                scratch_s16_.resize(chunk_samples);
                if (format_.bits_per_sample != 16) {
                    throw std::runtime_error(
                        "ALSA S16 container received non-16-bit logical PCM");
                }
                pack_s16_exact(samples + base, chunk_samples, scratch_s16_.data());
                write_ptr = scratch_s16_.data();
            } else if (pcm_container_format_ == SND_PCM_FORMAT_S24_3LE) {
                if (format_.bits_per_sample != 24) {
                    throw std::runtime_error(
                        "ALSA S24_3LE container received non-24-bit logical PCM");
                }
                scratch_s24_.resize(chunk_samples * 3u);
                for (std::size_t i = 0; i < chunk_samples; ++i) {
                    const std::int64_t v = static_cast<std::int64_t>(samples[base + i]);
                    if (v > 8388607 || v < -8388608) {
                        throw std::runtime_error(
                            "ALSA S24_3LE packing received an out-of-range logical PCM sample");
                    }
                    const std::uint32_t u =
                        static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
                    scratch_s24_[i * 3u + 0u] = static_cast<unsigned char>(u & 0xFFu);
                    scratch_s24_[i * 3u + 1u] = static_cast<unsigned char>((u >> 8) & 0xFFu);
                    scratch_s24_[i * 3u + 2u] = static_cast<unsigned char>((u >> 16) & 0xFFu);
                }
                write_ptr = scratch_s24_.data();
            } else {
                scratch_s32_.resize(chunk_samples);
                const bool s24_container = pcm_container_format_ == SND_PCM_FORMAT_S24_LE;
                const bool s32_carrying_24 =
                    pcm_container_format_ == SND_PCM_FORMAT_S32_LE &&
                    format_.bits_per_sample == 24;
                if (s24_container && format_.bits_per_sample != 24) {
                    throw std::runtime_error(
                        "ALSA S24_LE container received non-24-bit logical PCM");
                }
                if (pcm_container_format_ == SND_PCM_FORMAT_S32_LE &&
                    format_.bits_per_sample != 24 && format_.bits_per_sample != 32) {
                    throw std::runtime_error(
                        "ALSA S32_LE container received unsupported logical PCM precision");
                }
                for (std::size_t i = 0; i < chunk_samples; ++i) {
                    std::int64_t value = static_cast<std::int64_t>(samples[base + i]);
                    if (format_.bits_per_sample == 24) {
                        if (value > 8388607 || value < -8388608) {
                            throw std::runtime_error(
                                "ALSA 24-bit packing received an out-of-range logical PCM sample");
                        }
                        if (s32_carrying_24) value *= 256;
                    }
                    if (value > INT32_MAX || value < INT32_MIN) {
                        throw std::runtime_error(
                            "ALSA S32 packing received an out-of-range logical PCM sample");
                    }
                    scratch_s32_[i] = static_cast<std::int32_t>(value);
                }
                write_ptr = scratch_s32_.data();
            }
        }

        const bool tracing_first_write =
            diagnostic_enabled_ && !diagnostic_first_write_succeeded_;
        const bool first_write_attempt =
            tracing_first_write && !diagnostic_first_write_attempted_;
        const std::string first_write_pre_state = first_write_attempt
            ? pcm_state_and_delay(handle_) : std::string();
        const auto write_started = tracing_first_write
            ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        const snd_pcm_sframes_t result =
            snd_pcm_writei(handle_, write_ptr, frames_to_write);
        if (first_write_attempt) {
            diagnostic_first_write_attempted_ = true;
            Logger::instance().debug(
                "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                " first_music_write_attempt since_ready_us=" +
                std::to_string(elapsed_us(diagnostic_open_ready_at_, write_started)) +
                " requested_frames=" +
                std::to_string(static_cast<unsigned long long>(frames_to_write)) +
                " result=" + std::to_string(static_cast<long long>(result)) +
                " write_call_us=" +
                std::to_string(elapsed_us(write_started, DiagnosticClock::now())) +
                " pre_" + first_write_pre_state);
        }
        if (result == -EPIPE) {
            if (diagnostic_enabled_) {
                ++diagnostic_epipe_count_;
                if (diagnostic_epipe_count_ <= 8) {
                    Logger::instance().debug(
                        "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                        " EPIPE count=" + std::to_string(diagnostic_epipe_count_) +
                        " since_ready_us=" +
                        std::to_string(elapsed_us(diagnostic_open_ready_at_, DiagnosticClock::now())) +
                        " first_write_succeeded=" +
                        (diagnostic_first_write_succeeded_ ? "yes" : "no") +
                        " " + pcm_state_and_delay(handle_));
                }
            }
            if (!diagnostic_enabled_) {
                Logger::instance().debug("ALSA underrun, preparing again");
            }
            check_alsa(snd_pcm_prepare(handle_),
                       "snd_pcm_prepare failed after underrun");
            if (diagnostic_enabled_ && diagnostic_epipe_count_ <= 8) {
                Logger::instance().debug(
                    "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                    " EPIPE recovered " + pcm_state_and_delay(handle_));
            }
            continue;
        }
        if (result == -ESTRPIPE) {
            if (diagnostic_enabled_) {
                ++diagnostic_suspend_count_;
                if (diagnostic_suspend_count_ <= 8) {
                    Logger::instance().debug(
                        "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                        " ESTRPIPE count=" + std::to_string(diagnostic_suspend_count_) +
                        " " + pcm_state_and_delay(handle_));
                }
            }
            if (!diagnostic_enabled_) {
                Logger::instance().debug("ALSA suspended stream, trying resume");
            }
            recover_suspended_stream(handle_);
            continue;
        }
        if (result < 0) {
            throw std::runtime_error(std::string("snd_pcm_writei failed: ") +
                                     snd_strerror(static_cast<int>(result)));
        }
        if (tracing_first_write && result > 0) {
            diagnostic_first_write_succeeded_ = true;
            const auto first_write_done = DiagnosticClock::now();
            Logger::instance().debug(
                "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                " first_music_write_success written_frames=" +
                std::to_string(static_cast<long long>(result)) +
                " requested_frames=" +
                std::to_string(static_cast<unsigned long long>(frames_to_write)) +
                " write_call_us=" + std::to_string(elapsed_us(write_started, first_write_done)) +
                " since_ready_us=" +
                std::to_string(elapsed_us(diagnostic_open_ready_at_, first_write_done)) +
                " since_open_begin_us=" +
                std::to_string(elapsed_us(diagnostic_open_started_at_, first_write_done)) +
                " " + pcm_state_and_delay(handle_));
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
        if (diagnostic_enabled_) {
            Logger::instance().debug(
                "ALSA trace session=" + std::to_string(diagnostic_session_id_) +
                " close since_ready_us=" +
                std::to_string(elapsed_us(diagnostic_open_ready_at_, DiagnosticClock::now())) +
                " first_write_attempted=" +
                (diagnostic_first_write_attempted_ ? "yes" : "no") +
                " first_write_succeeded=" +
                (diagnostic_first_write_succeeded_ ? "yes" : "no") +
                " EPIPE_total=" + std::to_string(diagnostic_epipe_count_) +
                " ESTRPIPE_total=" + std::to_string(diagnostic_suspend_count_) +
                " start_silence_remaining_frames=" +
                std::to_string(start_silence_remaining_frames_));
        }
        Logger::instance().debug("Closing ALSA device (drop + close)");
        snd_pcm_drop(handle_);
        snd_pcm_close(handle_);
        handle_ = nullptr;
        pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
        accepted_sample_rate_ = 0;
        active_significant_bits_ = 0;
        output_format_ = PcmOutputFormat::S16;
    }
    scratch_start_silence_.clear();
    start_silence_total_frames_ = 0;
    start_silence_remaining_frames_ = 0;
    start_silence_started_ = false;
    start_silence_recoveries_ = 0;
    hw_pause_supported_ = false;
    hw_pause_active_ = false;
    software_resume_check_pending_ = false;
    diagnostic_enabled_ = false;
    diagnostic_session_id_ = 0;
    diagnostic_first_write_attempted_ = false;
    diagnostic_first_write_succeeded_ = false;
    diagnostic_epipe_count_ = 0;
    diagnostic_suspend_count_ = 0;
}

void AlsaPcmBackend::set_24bit_container_mode(Alsa24BitContainerMode mode) {
    format_24bit_mode_ = mode;
}

void AlsaPcmBackend::set_start_silence_ms(unsigned milliseconds) {
    start_silence_ms_ = std::min(milliseconds, 2000u);
}

std::string AlsaPcmBackend::active_output_report() const {
    if (pcm_container_format_ == SND_PCM_FORMAT_UNKNOWN || accepted_sample_rate_ == 0) {
        return std::string();
    }
    std::ostringstream ss;
    ss << "Device: " << (device_name_.empty() ? std::string("unknown") : device_name_) << '\n';
    if (device_name_.compare(0, 7, "plughw:") == 0) {
        ss << "ALSA path: Plug\n";
    } else if (device_name_.compare(0, 3, "hw:") == 0) {
        ss << "ALSA path: Direct\n";
    }
    ss << "Requested PCM: " << pcm_output_format_name(output_format_) << " / "
       << format_.sample_rate << " Hz / " << static_cast<unsigned>(format_.channels) << " ch" << '\n';
    ss << "Container: " << format_name_or_unknown(pcm_container_format_) << '\n';
    if (!pcm_output_is_floating(output_format_) && active_significant_bits_ > 0) {
        ss << "ALSA significant bits: " << active_significant_bits_ << "-bit\n";
    }
    ss << "Rate: " << accepted_sample_rate_
       << (accepted_sample_rate_ == format_.sample_rate ? " Hz (exact)" : " Hz (near)") << '\n';
    const AlsaBufferPolicy target_policy = alsa_buffer_policy_for_sample_rate(format_.sample_rate);
    ss << "Period / buffer target: "
       << static_cast<unsigned long long>(target_policy.period_frames)
       << "/" << static_cast<unsigned long long>(target_policy.buffer_frames) << '\n';
    ss << "Period / buffer actual: "
       << static_cast<unsigned long long>(period_frames_)
       << "/" << static_cast<unsigned long long>(buffer_frames_);
    return ss.str();
}

AlsaProbeMatrix AlsaPcmBackend::probe_device_format_matrix(
    const std::string& device_name,
    const std::string& plug_device_name) {
    const std::vector<unsigned> rates = {
        44100, 48000, 88200, 96000, 176400, 192000,
        352800, 384000, 705600, 768000, 1411200, 1536000
    };
    const std::vector<snd_pcm_format_t> formats = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_FLOAT,
        SND_PCM_FORMAT_FLOAT64
    };

    AlsaProbeMatrix matrix;
    matrix.device_name = device_name.empty() ? std::string("default") : device_name;
    matrix.plug_device_name = plug_device_name;
    matrix.sample_rates = rates;
    for (const snd_pcm_format_t format : formats) {
        matrix.format_names.push_back(format_name_or_unknown(format));
    }

    for (const snd_pcm_format_t format : formats) {
        for (const unsigned rate : rates) {
            AlsaProbeCell cell;
            cell.format_name = format_name_or_unknown(format);
            cell.sample_rate = rate;
            cell.supported = probe_pcm_format_rate(
                matrix.device_name, format, rate);
            matrix.cells.push_back(cell);
        }
    }

    if (!matrix.plug_device_name.empty()) {
        matrix.plug_float32_supported.reserve(rates.size());
        matrix.plug_float64_supported.reserve(rates.size());
        for (const unsigned rate : rates) {
            matrix.plug_float32_supported.push_back(probe_pcm_format_rate(
                matrix.plug_device_name, SND_PCM_FORMAT_FLOAT, rate));
            matrix.plug_float64_supported.push_back(probe_pcm_format_rate(
                matrix.plug_device_name, SND_PCM_FORMAT_FLOAT64, rate));
        }
    }

    return matrix;
}

} // namespace pcmtp
