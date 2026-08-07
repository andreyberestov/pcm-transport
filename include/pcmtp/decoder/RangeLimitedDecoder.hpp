// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

class RangeLimitedDecoder final : public IAudioDecoder {
public:
    RangeLimitedDecoder(std::unique_ptr<IAudioDecoder> inner, std::uint64_t start_sample, std::uint64_t end_sample);
    void open(const std::string& path) override;
    void open_at_sample(const std::string& path, std::uint64_t sample_index) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    PresentationEndKind presentation_end_kind() const noexcept override;
    TransportTruncationKind transport_truncation_kind() const noexcept override;
    bool seek_to_sample(std::uint64_t sample_index) override;
    void request_abort() override;
    void request_stop_after_current_segment(std::uint64_t segment_end_sample) override;
private:
    std::uint64_t remaining_samples_per_channel() const;
    std::unique_ptr<IAudioDecoder> inner_;
    std::uint64_t start_sample_ = 0;
    std::uint64_t end_sample_ = 0;
    std::uint64_t track_length_samples_ = 0;
    std::uint64_t consumed_samples_per_channel_ = 0;
    std::atomic<std::uint64_t> requested_end_sample_{
        std::numeric_limits<std::uint64_t>::max()};
    bool opened_ = false;
};

} // namespace pcmtp
