// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/decoder/SampleBoundary.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <string>

namespace pcmtp {
namespace {

std::uint64_t rounded_timeline_samples(std::int64_t duration,
                                       int time_base_num,
                                       int time_base_den,
                                       std::uint32_t sample_rate) {
    if (duration <= 0 || time_base_num <= 0 || time_base_den <= 0 || sample_rate == 0) {
        return 0;
    }
    const long double value = static_cast<long double>(duration) *
                              static_cast<long double>(time_base_num) *
                              static_cast<long double>(sample_rate) /
                              static_cast<long double>(time_base_den);
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    if (!(value > 0.0L) || value > maximum - 0.5L) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::floor(value + 0.5L));
}

std::uint64_t rounded_rate_converted_samples(std::uint64_t samples,
                                             std::uint32_t source_sample_rate,
                                             std::uint32_t output_sample_rate) {
    if (samples == 0 || source_sample_rate == 0 || output_sample_rate == 0) {
        return 0;
    }
    const long double value = static_cast<long double>(samples) *
                              static_cast<long double>(output_sample_rate) /
                              static_cast<long double>(source_sample_rate);
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    if (!(value > 0.0L) || value > maximum - 0.5L) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::floor(value + 0.5L));
}

bool multiply_checked(std::uint64_t left,
                      std::uint64_t right,
                      std::uint64_t* result) {
    if (result == nullptr) {
        return false;
    }
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

std::uint64_t greatest_common_divisor(std::uint64_t left,
                                      std::uint64_t right) {
    while (right != 0) {
        const std::uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

bool exact_timeline_samples(std::int64_t duration,
                            int time_base_num,
                            int time_base_den,
                            std::uint32_t sample_rate,
                            std::uint64_t* samples) {
    if (samples == nullptr || duration <= 0 || time_base_num <= 0 ||
        time_base_den <= 0 || sample_rate == 0) {
        return false;
    }

    std::uint64_t duration_factor = static_cast<std::uint64_t>(duration);
    std::uint64_t time_factor = static_cast<std::uint64_t>(time_base_num);
    std::uint64_t rate_factor = sample_rate;
    std::uint64_t denominator = static_cast<std::uint64_t>(time_base_den);

    std::uint64_t divisor = greatest_common_divisor(duration_factor, denominator);
    duration_factor /= divisor;
    denominator /= divisor;

    divisor = greatest_common_divisor(time_factor, denominator);
    time_factor /= divisor;
    denominator /= divisor;

    divisor = greatest_common_divisor(rate_factor, denominator);
    rate_factor /= divisor;
    denominator /= divisor;

    if (denominator != 1) {
        return false;
    }

    std::uint64_t product = 0;
    if (!multiply_checked(duration_factor, time_factor, &product) ||
        !multiply_checked(product, rate_factor, &product) || product == 0) {
        return false;
    }
    *samples = product;
    return true;
}

bool demuxer_has_token(const std::string& names, const std::string& token) {
    std::size_t start = 0;
    while (start <= names.size()) {
        const std::size_t comma = names.find(',', start);
        const std::size_t length = comma == std::string::npos
            ? names.size() - start
            : comma - start;
        if (names.compare(start, length, token) == 0) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

bool mov_family_demuxer(const std::string& demuxer_name) {
    static const char* const names[] = {"mov", "mp4", "m4a", "3gp", "3g2", "mj2"};
    return std::any_of(std::begin(names), std::end(names), [&](const char* name) {
        return demuxer_has_token(demuxer_name, name);
    });
}

bool starts_with(const std::string& value, const char* prefix) {
    if (prefix == nullptr) {
        return false;
    }
    const std::size_t length = std::char_traits<char>::length(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

enum class DemuxerMatch {
    ExactToken,
    MovFamily
};

enum class CodecMatch {
    Exact,
    Prefix
};

struct DeclaredSampleCountRule {
    DemuxerMatch demuxer_match;
    const char* demuxer_name;
    CodecMatch codec_match;
    const char* codec_name;
    bool require_direct_sample_time_base;
};

bool rule_matches(const DeclaredSampleCountRule& rule,
                  const LibavStreamBoundaryFacts& facts) {
    const bool demuxer_matches = rule.demuxer_match == DemuxerMatch::MovFamily
        ? mov_family_demuxer(facts.demuxer_name)
        : demuxer_has_token(facts.demuxer_name, rule.demuxer_name);
    if (!demuxer_matches) {
        return false;
    }
    const bool codec_matches = rule.codec_match == CodecMatch::Prefix
        ? starts_with(facts.codec_name, rule.codec_name)
        : facts.codec_name == rule.codec_name;
    if (!codec_matches) {
        return false;
    }
    return !rule.require_direct_sample_time_base ||
           (facts.time_base_num == 1 && facts.time_base_den > 0 &&
            static_cast<std::uint32_t>(facts.time_base_den) == facts.sample_rate);
}

SampleExtent exact_presentation_extent(std::uint64_t samples,
                                       SampleExtentSource source,
                                       ExactPresentationDrainPolicy drain_policy =
                                           ExactPresentationDrainPolicy::DecoderEofMatchesPresentation) {
    SampleExtent extent;
    if (samples == 0) {
        return extent;
    }
    extent.samples = samples;
    extent.kind = SampleExtentKind::ExactPresentationSpan;
    extent.source = source;
    extent.exact_presentation_drain_policy = drain_policy;
    return extent;
}

SampleExtent demuxer_declared_sample_count_evidence(
    const LibavStreamBoundaryFacts& facts,
    std::uint64_t exact_samples) {
    // These demuxer/codec pairs expose a format-level exact presentation
    // count through AVStream::duration. Rules that promise a direct sample
    // count also require a 1/sample_rate time base. Keep this knowledge in the
    // boundary-evidence layer so transport and GUI remain format-agnostic.
    static const DeclaredSampleCountRule rules[] = {
        {DemuxerMatch::ExactToken, "wv",  CodecMatch::Exact,  "wavpack", true},
        {DemuxerMatch::ExactToken, "ape", CodecMatch::Exact,  "ape",     true},
        {DemuxerMatch::ExactToken, "tak", CodecMatch::Exact,  "tak",     true},
        {DemuxerMatch::ExactToken, "w64", CodecMatch::Prefix, "pcm_",    true},
        {DemuxerMatch::MovFamily,  "",    CodecMatch::Exact,  "alac",    false}
    };

    if (facts.initial_padding != 0 || facts.trailing_padding != 0) {
        return SampleExtent{};
    }
    const bool matched = std::any_of(std::begin(rules), std::end(rules),
                                     [&](const DeclaredSampleCountRule& rule) {
        return rule_matches(rule, facts);
    });
    return matched
        ? exact_presentation_extent(exact_samples,
                                    SampleExtentSource::DemuxerDeclaredSampleCount)
        : SampleExtent{};
}

SampleExtent mov_aac_presentation_evidence(
    const LibavStreamBoundaryFacts& facts,
    std::uint64_t exact_samples) {
    if (!facts.mov_aac_exact_presentation_evidence ||
        !mov_family_demuxer(facts.demuxer_name) ||
        facts.codec_name != "aac") {
        return SampleExtent{};
    }
    return exact_presentation_extent(
        exact_samples,
        SampleExtentSource::MovAacPresentationBoundary,
        facts.mov_aac_exact_presentation_drain_policy);
}

int extent_strength(SampleExtentKind kind) {
    switch (kind) {
    case SampleExtentKind::ExactPresentationSpan:
        return 3;
    case SampleExtentKind::ExactDecodedSpan:
        return 2;
    case SampleExtentKind::EstimatedTimeline:
        return 1;
    case SampleExtentKind::Unknown:
    default:
        return 0;
    }
}

SampleExtent select_stronger_sample_extent_impl(const SampleExtent& current,
                                                const SampleExtent& candidate) {
    if (candidate.samples == 0 ||
        extent_strength(candidate.kind) < extent_strength(current.kind)) {
        return current;
    }
    if (extent_strength(candidate.kind) > extent_strength(current.kind)) {
        return candidate;
    }
    // Conflicting equally strong evidence must not silently select a bound.
    if (sample_extent_supports_bounded_transport(candidate.kind) &&
        current.samples != 0 && current.samples != candidate.samples) {
        return SampleExtent{};
    }
    return candidate;
}

} // namespace

SampleExtent select_stronger_sample_extent(const SampleExtent& current,
                                           const SampleExtent& candidate) {
    return select_stronger_sample_extent_impl(current, candidate);
}

bool sample_extent_supports_bounded_transport(SampleExtentKind kind) noexcept {
    return kind == SampleExtentKind::ExactDecodedSpan ||
           kind == SampleExtentKind::ExactPresentationSpan;
}

bool sample_extent_supports_gapless_presentation(SampleExtentKind kind) noexcept {
    return kind == SampleExtentKind::ExactPresentationSpan;
}

bool libav_stream_supports_trusted_decoder_eof(
    const LibavStreamBoundaryFacts& facts) noexcept {
    if (!facts.stream_info_complete ||
        facts.duration <= 0 ||
        facts.time_base_num <= 0 ||
        facts.time_base_den <= 0 ||
        facts.sample_rate == 0 ||
        facts.audio_stream_count != 1) {
        return false;
    }

    // The evidence source records why a fully drained decoder EOF may be used
    // as the presentation boundary. Format knowledge stays in this boundary
    // policy layer; transport and gapless code consume only PresentationEndKind.
    switch (facts.decoder_eof_evidence_source) {
    case DecoderEofEvidenceSource::OggTerminalEos:
        return facts.stream_count == 1 &&
               demuxer_has_token(facts.demuxer_name, "ogg") &&
               (facts.codec_name == "vorbis" || facts.codec_name == "opus");
    case DecoderEofEvidenceSource::MovAacTerminalDiscard:
        return facts.mov_aac_exact_presentation_evidence &&
               mov_family_demuxer(facts.demuxer_name) &&
               facts.codec_name == "aac";
    case DecoderEofEvidenceSource::None:
        return false;
    }

    return false;
}

PresentationEndKind presentation_end_kind_for_output(
    const SampleExtent& source_extent,
    const SampleExtent& output_extent,
    bool source_supports_trusted_decoder_eof) noexcept {
    if (output_extent.kind == SampleExtentKind::ExactPresentationSpan) {
        return PresentationEndKind::ExactSampleSpan;
    }
    if (source_supports_trusted_decoder_eof) {
        return PresentationEndKind::TrustedDecoderEof;
    }
    if (source_extent.kind == SampleExtentKind::ExactPresentationSpan &&
        source_extent.exact_presentation_drain_policy ==
            ExactPresentationDrainPolicy::DecoderEofMatchesPresentation &&
        output_extent.kind == SampleExtentKind::EstimatedTimeline &&
        output_extent.source == SampleExtentSource::RateConvertedTimeline) {
        return PresentationEndKind::TrustedDecoderEof;
    }
    return PresentationEndKind::Unknown;
}

bool presentation_end_supports_gapless(PresentationEndKind kind) noexcept {
    return kind == PresentationEndKind::ExactSampleSpan ||
           kind == PresentationEndKind::TrustedDecoderEof;
}

SampleExtent classify_libav_stream_extent(const LibavStreamBoundaryFacts& facts) {
    SampleExtent selected;
    selected.samples = rounded_timeline_samples(facts.duration,
                                                facts.time_base_num,
                                                facts.time_base_den,
                                                facts.sample_rate);
    if (selected.samples == 0) {
        return selected;
    }

    selected.kind = SampleExtentKind::EstimatedTimeline;
    selected.source = SampleExtentSource::DemuxerStreamTimeline;

    std::uint64_t exact_samples = 0;
    if (!facts.stream_info_complete ||
        !exact_timeline_samples(facts.duration,
                                facts.time_base_num,
                                facts.time_base_den,
                                facts.sample_rate,
                                &exact_samples)) {
        return selected;
    }

    selected = select_stronger_sample_extent(
        selected, demuxer_declared_sample_count_evidence(facts, exact_samples));
    selected = select_stronger_sample_extent(
        selected, mov_aac_presentation_evidence(facts, exact_samples));
    return selected;
}

SampleExtent estimated_container_extent(std::int64_t duration,
                                        int time_base_num,
                                        int time_base_den,
                                        std::uint32_t sample_rate) {
    SampleExtent extent;
    extent.samples = rounded_timeline_samples(duration,
                                              time_base_num,
                                              time_base_den,
                                              sample_rate);
    if (extent.samples > 0) {
        extent.kind = SampleExtentKind::EstimatedTimeline;
        extent.source = SampleExtentSource::ContainerTimeline;
    }
    return extent;
}

SampleExtent transform_sample_extent_for_output(
    const SampleExtent& source_extent,
    std::uint32_t source_sample_rate,
    std::uint32_t output_sample_rate) {
    if (source_sample_rate == 0 || output_sample_rate == 0 ||
        source_sample_rate == output_sample_rate) {
        return source_extent;
    }

    SampleExtent transformed;
    transformed.exact_presentation_drain_policy =
        source_extent.exact_presentation_drain_policy;
    transformed.samples = rounded_rate_converted_samples(
        source_extent.samples, source_sample_rate, output_sample_rate);
    if (transformed.samples > 0) {
        // A rate-converted frame count depends on the streaming resampler,
        // including its delay and final drain. The source proof remains valid
        // in the source domain, but the rounded output timeline is display-only.
        transformed.kind = SampleExtentKind::EstimatedTimeline;
        transformed.source = SampleExtentSource::RateConvertedTimeline;
    }
    return transformed;
}

const char* sample_extent_kind_name(SampleExtentKind kind) noexcept {
    switch (kind) {
    case SampleExtentKind::EstimatedTimeline:
        return "estimated timeline";
    case SampleExtentKind::ExactDecodedSpan:
        return "exact decoded span";
    case SampleExtentKind::ExactPresentationSpan:
        return "exact presentation span";
    case SampleExtentKind::Unknown:
    default:
        return "unknown";
    }
}


const char* sample_extent_source_name(SampleExtentSource source) noexcept {
    switch (source) {
    case SampleExtentSource::ContainerTimeline:
        return "container timeline";
    case SampleExtentSource::DemuxerStreamTimeline:
        return "demuxer stream timeline";
    case SampleExtentSource::RateConvertedTimeline:
        return "rate-converted timeline estimate";
    case SampleExtentSource::DemuxerDeclaredSampleCount:
        return "demuxer-declared sample count";
    case SampleExtentSource::NativeHeader:
        return "native header";
    case SampleExtentSource::PcmDataSize:
        return "PCM data size";
    case SampleExtentSource::CodecGaplessMetadata:
        return "codec gapless metadata";
    case SampleExtentSource::MovAacPresentationBoundary:
        return "verified MOV AAC presentation boundary";
    case SampleExtentSource::AiffPcmData:
        return "verified AIFF PCM data";
    case SampleExtentSource::AuPcmData:
        return "verified AU PCM data";
    case SampleExtentSource::CafLpcmData:
        return "verified CAF LPCM data";
    case SampleExtentSource::TtaSampleCount:
        return "verified TTA sample count";
    case SampleExtentSource::DsfSampleCount:
        return "verified DSF sample count";
    case SampleExtentSource::DffDsdData:
        return "verified DFF DSD data";
    case SampleExtentSource::CueIndex:
        return "CUE index";
    case SampleExtentSource::None:
    default:
        return "none";
    }
}

} // namespace pcmtp
