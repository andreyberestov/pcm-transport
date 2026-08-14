// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

enum class GaplessBoundaryMode {
    ExactRange,
    DecoderEof
};

struct GaplessTrackSpec {
    std::string path;
    AudioFormat format{};
    std::uint64_t start_sample = 0;
    // ExactRange: exact half-open transport end.
    // DecoderEof: estimated end used only for display/prefetch planning.
    std::uint64_t planned_end_sample = 0;
    bool start_sample_known = true;
    bool exact_end_sample_known = false;
    GaplessBoundaryMode boundary_mode = GaplessBoundaryMode::ExactRange;
    bool native_flac = false;
    std::uint32_t forced_output_sample_rate = 0;
    std::uint16_t forced_output_bits_per_sample = 0;
    std::string resample_quality = "maximum";
    std::string bitdepth_quality = "tpdf_hp";
    ExternalAudioInfo known_external_info{};
    bool has_known_external_info = false;
};

class GaplessChainDecoder final : public IAudioDecoder {
public:
    explicit GaplessChainDecoder(std::vector<GaplessTrackSpec> tracks, std::uint64_t first_track_offset = 0);
    ~GaplessChainDecoder() override;

    void open(const std::string& path) override;
    void open_at_sample(const std::string& path, std::uint64_t sample_index) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    PresentationEndKind presentation_end_kind() const noexcept override;
    DecoderSegmentPosition segment_position() const noexcept override;
    TransportTruncationKind transport_truncation_kind() const noexcept override;
    ResamplerRuntimeKind resampler_runtime_kind() const noexcept override;
    bool seek_to_sample(std::uint64_t sample_index) override;
    void request_abort() override;
    void request_stop_after_current_segment(std::uint64_t segment_end_sample) override;
    void request_stop_after_segment(std::size_t segment_index) override;

private:
    struct PreparedNext {
        std::size_t index = 0;
        std::unique_ptr<IAudioDecoder> decoder;
        PcmBuffer prebuffer;
        ResamplerRuntimeKind resampler_runtime_kind =
            ResamplerRuntimeKind::NotUsed;
        bool ready = false;
        bool failed = false;
    };

    std::unique_ptr<IAudioDecoder> create_decoder_for_track(const GaplessTrackSpec& spec) const;
    void open_decoder_at_local_offset(IAudioDecoder& decoder,
                                      const GaplessTrackSpec& spec,
                                      std::uint64_t local_offset) const;
    void open_current_decoder(std::uint64_t offset);
    void close_prepare_thread();
    void maybe_prepare_next();
    void prepare_next_worker(std::size_t index, std::uint64_t generation);
    enum class SwitchResult { Switched, NoNext };

    SwitchResult switch_to_next_track();
    std::uint64_t track_length(std::size_t index) const;
    bool track_uses_exact_range(std::size_t index) const;
    bool uses_decoder_eof_boundaries() const noexcept;
    bool stop_requested_after_current_segment() const;
    std::uint64_t current_chain_position() const;
    bool requested_segment_end_reached() const;
    std::uint64_t prepare_threshold_frames(std::size_t index) const;
    std::size_t prebuffer_samples(std::size_t index) const;

    std::vector<GaplessTrackSpec> tracks_;
    std::vector<std::uint64_t> track_offsets_;
    std::size_t current_index_ = 0;
    std::uint64_t current_track_position_ = 0;
    std::uint64_t first_track_offset_ = 0;
    std::uint64_t total_samples_per_channel_ = 0;
    AudioFormat format_{};
    std::unique_ptr<IAudioDecoder> current_decoder_;
    mutable std::mutex decoder_mutex_;
    std::atomic<ResamplerRuntimeKind> resampler_runtime_kind_{
        ResamplerRuntimeKind::NotUsed};
    bool opened_ = false;
    bool reached_eof_ = false;
    std::atomic<std::uint64_t> requested_end_sample_{
        std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::size_t> requested_last_segment_index_{
        std::numeric_limits<std::size_t>::max()};

    mutable std::mutex prepare_mutex_;
    std::thread prepare_thread_;
    PreparedNext prepared_;
    PcmBuffer current_prebuffer_;
    std::size_t current_prebuffer_offset_ = 0;
    bool preparing_ = false;
    IAudioDecoder* preparing_decoder_ = nullptr;
    std::atomic<bool> prepare_cancel_requested_{false};
    std::atomic<std::uint64_t> prepare_generation_{0};
    std::atomic<bool> abort_requested_{false};

};

} // namespace pcmtp
