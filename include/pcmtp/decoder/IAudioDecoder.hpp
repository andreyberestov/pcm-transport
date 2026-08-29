// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "pcmtp/core/PcmTypes.hpp"
#include "pcmtp/decoder/SampleBoundary.hpp"

namespace pcmtp {

enum class TransportTruncationKind {
    None,
    ExactSampleBoundary,
    DecoderSegmentBoundary
};

enum class ResamplerRuntimeKind {
    NotUsed,
    Initializing,
    SoXr,
    FfmpegSwr
};

struct DecoderSegmentPosition {
    bool valid = false;
    std::size_t index = 0;
    std::uint64_t samples_per_channel = 0;
};

struct DecoderRuntimeStateSnapshot {
    std::uint64_t generation = 0;
    ResamplerRuntimeKind resampler_runtime_kind = ResamplerRuntimeKind::NotUsed;
    Pcm16QuantizationRuntimeKind pcm16_quantization_runtime_kind =
        Pcm16QuantizationRuntimeKind::NotUsed;
    std::uint32_t pcm16_quantization_stage_count = 0;
    std::string source_codec_name;
    std::uint64_t encoded_bitrate_bps = 0;
    std::string decoder_implementation_name;
    DecoderPcmSampleKind decoded_pcm_sample_kind = DecoderPcmSampleKind::Unknown;
    std::uint16_t decoded_pcm_significant_bits = 0;
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual void open(const std::string& path) = 0;
    virtual void open_at_sample(const std::string& path, std::uint64_t sample_index) {
        open(path);
        if (sample_index > 0) {
            seek_to_sample(sample_index);
        }
    }
    virtual const AudioFormat& format() const = 0;
    virtual std::size_t read_samples(PcmSample* destination, std::size_t max_samples) = 0;
    virtual bool eof() const = 0;
    virtual std::uint64_t total_samples_per_channel() const = 0;
    virtual std::string source_path() const = 0;
    virtual PresentationEndKind presentation_end_kind() const noexcept {
        return PresentationEndKind::Unknown;
    }
    virtual DecoderSegmentPosition segment_position() const noexcept {
        return DecoderSegmentPosition{};
    }
    virtual TransportTruncationKind transport_truncation_kind() const noexcept {
        return TransportTruncationKind::None;
    }
    virtual ResamplerRuntimeKind resampler_runtime_kind() const noexcept {
        return ResamplerRuntimeKind::NotUsed;
    }
    virtual Pcm16QuantizationRuntimeKind pcm16_quantization_runtime_kind() const noexcept {
        return Pcm16QuantizationRuntimeKind::NotUsed;
    }
    virtual std::uint32_t pcm16_quantization_stage_count() const noexcept {
        return 0;
    }
    virtual std::string decoded_codec_name() const { return std::string(); }
    virtual DecoderPcmSampleKind decoded_pcm_sample_kind() const noexcept {
        return DecoderPcmSampleKind::Unknown;
    }
    virtual std::uint16_t decoded_pcm_significant_bits() const noexcept {
        return 0;
    }
    virtual std::uint64_t runtime_state_generation() const noexcept {
        return 0;
    }
    virtual DecoderRuntimeStateSnapshot runtime_state_snapshot() const {
        DecoderRuntimeStateSnapshot state;
        state.generation = runtime_state_generation();
        state.resampler_runtime_kind = resampler_runtime_kind();
        state.pcm16_quantization_runtime_kind = pcm16_quantization_runtime_kind();
        state.pcm16_quantization_stage_count = pcm16_quantization_stage_count();
        state.decoder_implementation_name = decoded_codec_name();
        state.decoded_pcm_sample_kind = decoded_pcm_sample_kind();
        state.decoded_pcm_significant_bits = decoded_pcm_significant_bits();
        return state;
    }
    virtual bool seek_to_sample(std::uint64_t sample_index) { (void)sample_index; return false; }
    virtual void request_abort() {}
    virtual void request_stop_after_current_segment(std::uint64_t segment_end_sample) {
        (void)segment_end_sample;
    }
    virtual void request_stop_after_segment(std::size_t segment_index) {
        (void)segment_index;
    }
};

} // namespace pcmtp
