// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>

namespace pcmtp {

// Describes what the sample count proves, independently of codec family.
// EstimatedTimeline is display-only. ExactDecodedSpan permits bounded
// transport but not separate-file gapless. ExactPresentationSpan permits both.
enum class SampleExtentKind {
    Unknown,
    EstimatedTimeline,
    ExactDecodedSpan,
    ExactPresentationSpan
};

// Separates a numerically exact presentation span from a decoder EOF that is
// trusted only after the decoder and any resampler have fully drained.
enum class PresentationEndKind {
    Unknown,
    ExactSampleSpan,
    TrustedDecoderEof
};

// Describes whether a source-domain exact presentation span may safely fall
// back to a fully drained decoder EOF after rate conversion removes the exact
// numeric output bound. Existing exact sources are decoder-EOF-safe; formats
// with encoder padding may require the exact source range unless separate EOF
// evidence proves that the decoder removes the terminal padding.
enum class ExactPresentationDrainPolicy {
    DecoderEofMatchesPresentation,
    ExactRangeRequired
};

// Records the evidence used to classify the extent. The transport and
// gapless planners consume SampleExtentKind and remain format-agnostic.
enum class SampleExtentSource {
    None,
    ContainerTimeline,
    DemuxerStreamTimeline,
    RateConvertedTimeline,
    DemuxerDeclaredSampleCount,
    NativeHeader,
    PcmDataSize,
    CodecGaplessMetadata,
    MovAacPresentationBoundary,
    AiffPcmData,
    AuPcmData,
    CafLpcmData,
    TtaSampleCount,
    DsfSampleCount,
    DffDsdData,
    CueIndex
};

// Records format-specific evidence that permits a fully drained decoder EOF to
// act as the presentation boundary without promoting an estimated timeline to
// an exact sample range.
enum class DecoderEofEvidenceSource {
    None,
    OggTerminalEos,
    MovAacTerminalDiscard
};

struct SampleExtent {
    std::uint64_t samples = 0;
    SampleExtentKind kind = SampleExtentKind::Unknown;
    SampleExtentSource source = SampleExtentSource::None;
    ExactPresentationDrainPolicy exact_presentation_drain_policy =
        ExactPresentationDrainPolicy::DecoderEofMatchesPresentation;
};

struct LibavStreamBoundaryFacts {
    std::string demuxer_name;
    std::string codec_name;
    std::int64_t duration = 0;
    int time_base_num = 0;
    int time_base_den = 0;
    std::uint32_t sample_rate = 0;
    int initial_padding = 0;
    int trailing_padding = 0;
    unsigned int stream_count = 0;
    unsigned int audio_stream_count = 0;
    bool stream_info_complete = false;
    bool mov_aac_exact_presentation_evidence = false;
    ExactPresentationDrainPolicy mov_aac_exact_presentation_drain_policy =
        ExactPresentationDrainPolicy::ExactRangeRequired;
    DecoderEofEvidenceSource decoder_eof_evidence_source =
        DecoderEofEvidenceSource::None;
};

bool sample_extent_supports_bounded_transport(SampleExtentKind kind) noexcept;
bool sample_extent_supports_gapless_presentation(SampleExtentKind kind) noexcept;
bool libav_stream_supports_trusted_decoder_eof(
    const LibavStreamBoundaryFacts& facts) noexcept;
PresentationEndKind presentation_end_kind_for_output(
    const SampleExtent& source_extent,
    const SampleExtent& output_extent,
    bool source_supports_trusted_decoder_eof = false) noexcept;
bool presentation_end_supports_gapless(PresentationEndKind kind) noexcept;

SampleExtent select_stronger_sample_extent(const SampleExtent& current,
                                           const SampleExtent& candidate);
SampleExtent classify_libav_stream_extent(const LibavStreamBoundaryFacts& facts);
SampleExtent estimated_container_extent(std::int64_t duration,
                                        int time_base_num,
                                        int time_base_den,
                                        std::uint32_t sample_rate);
SampleExtent transform_sample_extent_for_output(const SampleExtent& source_extent,
                                                std::uint32_t source_sample_rate,
                                                std::uint32_t output_sample_rate);

const char* sample_extent_kind_name(SampleExtentKind kind) noexcept;
const char* sample_extent_source_name(SampleExtentSource source) noexcept;

} // namespace pcmtp
