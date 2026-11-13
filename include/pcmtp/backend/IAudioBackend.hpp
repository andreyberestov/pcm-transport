// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

class AudioFormatUnsupportedError : public std::runtime_error {
public:
    explicit AudioFormatUnsupportedError(const std::string& message)
        : std::runtime_error(message) {}
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual void open(const std::string& device_name,
                      const AudioFormat& format,
                      PcmOutputFormat output_format) = 0;
    virtual PcmOutputFormat open_with_format_candidates(
        const std::string& device_name,
        const AudioFormat& base_format,
        const std::vector<PcmOutputFormat>& format_candidates) {
        if (format_candidates.empty()) {
            throw std::invalid_argument(
                "Audio backend received no output format candidates");
        }
        std::string last_unsupported;
        for (const PcmOutputFormat output_format : format_candidates) {
            AudioFormat candidate = base_format;
            const std::uint16_t integer_bits =
                pcm_output_integer_bits(output_format);
            candidate.bits_per_sample = integer_bits > 0 ? integer_bits : 32;
            try {
                open(device_name, candidate, output_format);
                return output_format;
            } catch (const AudioFormatUnsupportedError& ex) {
                last_unsupported = ex.what();
            }
        }
        throw AudioFormatUnsupportedError(
            last_unsupported.empty()
                ? std::string("Audio backend does not support the requested output format")
                : last_unsupported);
    }
    virtual std::size_t write_samples(const PcmBlock& samples,
                                      std::size_t offset,
                                      std::size_t sample_count) = 0;
    // One bounded startup-silence write. Returns true while silence remains.
    // Called from the playback worker before the first musical PCM block.
    virtual bool write_start_silence_chunk() { return false; }
    // Invoked only by the playback worker, never concurrently with write_samples().
    // False retains the existing software-pause behavior.
    virtual bool pause_device() { return false; }
    virtual void resume_device() {}
    virtual void resume_software_pause() {}
    virtual void log_transport_state(const char*) {}
    virtual void drain() = 0;
    virtual void close() = 0;
    virtual std::string active_output_report() const { return std::string(); }
};

} // namespace pcmtp
