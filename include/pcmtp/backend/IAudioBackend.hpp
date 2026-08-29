// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
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

    virtual void open(const std::string& device_name, const AudioFormat& format) = 0;
    virtual AudioFormat open_with_precision_candidates(
        const std::string& device_name,
        const AudioFormat& base_format,
        const std::vector<std::uint16_t>& precision_candidates) {
        if (precision_candidates.empty()) {
            throw std::invalid_argument(
                "Audio backend received no output precision candidates");
        }
        std::string last_unsupported;
        for (const std::uint16_t bits : precision_candidates) {
            if (bits != 16 && bits != 24 && bits != 32) {
                throw std::invalid_argument(
                    "Audio backend received an unsupported output precision");
            }
            AudioFormat candidate = base_format;
            candidate.bits_per_sample = bits;
            try {
                open(device_name, candidate);
                return candidate;
            } catch (const AudioFormatUnsupportedError& ex) {
                last_unsupported = ex.what();
            }
        }
        throw AudioFormatUnsupportedError(
            last_unsupported.empty()
                ? std::string("Audio backend does not support the requested output precision")
                : last_unsupported);
    }
    virtual std::size_t write_samples(const PcmSample* samples, std::size_t sample_count) = 0;
    virtual void drain() = 0;
    virtual void close() = 0;
    virtual std::string active_output_report() const { return std::string(); }
};

} // namespace pcmtp
