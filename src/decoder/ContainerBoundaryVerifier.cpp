// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/decoder/ContainerBoundaryVerifier.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>

namespace pcmtp {
namespace {

bool starts_with(const std::string& value, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool format_name_has_token(const std::string& names, const char* token) {
    std::size_t start = 0;
    while (start <= names.size()) {
        const std::size_t comma = names.find(',', start);
        const std::size_t end = comma == std::string::npos ? names.size() : comma;
        if (names.compare(start, end - start, token) == 0) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

bool read_exact(std::ifstream& input, unsigned char* data, std::size_t size) {
    input.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount()) == size;
}

std::uint16_t read_le16(const unsigned char* data) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8U));
}

std::uint32_t read_le32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t read_le64(const unsigned char* data) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8U) | static_cast<std::uint64_t>(data[index]);
    }
    return value;
}

std::uint16_t read_be16(const unsigned char* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_be32(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_be64(const unsigned char* data) {
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(data[index]);
    }
    return value;
}

std::uint32_t crc32_ieee_le_update(std::uint32_t crc,
                                   const unsigned char* data,
                                   std::size_t size) {
    constexpr std::uint32_t kPolynomial = 0xEDB88320U;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (kPolynomial & mask);
        }
    }
    return crc;
}

std::uint32_t crc32_ieee_le(const unsigned char* data, std::size_t size) {
    return crc32_ieee_le_update(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU;
}

bool open_binary_with_size(const std::string& path,
                           std::ifstream* input,
                           std::uint64_t* file_size) {
    if (input == nullptr || file_size == nullptr) {
        return false;
    }
    input->open(path.c_str(), std::ios::binary);
    if (!*input) {
        return false;
    }
    input->seekg(0, std::ios::end);
    const std::streamoff size = input->tellg();
    if (size <= 0) {
        return false;
    }
    *file_size = static_cast<std::uint64_t>(size);
    input->seekg(0, std::ios::beg);
    return static_cast<bool>(*input);
}

bool seek_binary(std::ifstream& input, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return static_cast<bool>(input);
}

bool checked_add_u64(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t* result) {
    if (result == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checked_multiply_u64(std::uint64_t left,
                          std::uint64_t right,
                          std::uint64_t* result) {
    if (result == nullptr ||
        (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

bool fixed_linear_pcm_codec_name(const std::string& codec_name) {
    if (!starts_with(codec_name, "pcm_")) {
        return false;
    }
    return codec_name.find("alaw") == std::string::npos &&
           codec_name.find("mulaw") == std::string::npos &&
           codec_name.find("dvd") == std::string::npos &&
           codec_name.find("bluray") == std::string::npos;
}

bool classic_aiff_pcm_codec_name(const std::string& codec_name) {
    return codec_name == "pcm_s8" || codec_name == "pcm_s16be" ||
           codec_name == "pcm_s24be" || codec_name == "pcm_s32be";
}

const char* au_pcm_codec_name_for_encoding(std::uint32_t encoding) {
    switch (encoding) {
        case 2: return "pcm_s8";
        case 3: return "pcm_s16be";
        case 4: return "pcm_s24be";
        case 5: return "pcm_s32be";
        case 6: return "pcm_f32be";
        case 7: return "pcm_f64be";
        default: return nullptr;
    }
}

SampleExtent exact_presentation_file_extent(std::uint64_t samples,
                                            SampleExtentSource source) {
    SampleExtent extent;
    if (samples == 0) {
        return extent;
    }
    extent.samples = samples;
    extent.kind = SampleExtentKind::ExactPresentationSpan;
    extent.source = source;
    extent.exact_presentation_drain_policy =
        ExactPresentationDrainPolicy::DecoderEofMatchesPresentation;
    return extent;
}

SampleExtent exact_decoded_file_extent(std::uint64_t samples,
                                       SampleExtentSource source) {
    SampleExtent extent;
    if (samples == 0) {
        return extent;
    }
    extent.samples = samples;
    extent.kind = SampleExtentKind::ExactDecodedSpan;
    extent.source = source;
    return extent;
}

SampleExtent verify_aiff_pcm_extent(const std::string& path,
                                    const ContainerBoundaryFacts& facts) {
    const std::string codec_name = lower_copy(facts.codec_name);
    if (facts.sample_rate == 0 ||
        facts.channels == 0 ||
        !classic_aiff_pcm_codec_name(codec_name)) {
        return SampleExtent{};
    }

    std::ifstream input;
    std::uint64_t file_size = 0;
    if (!open_binary_with_size(path, &input, &file_size) || file_size < 12) {
        return SampleExtent{};
    }
    unsigned char form[12]{};
    if (!read_exact(input, form, sizeof(form)) ||
        std::memcmp(form, "FORM", 4) != 0 ||
        std::memcmp(form + 8, "AIFF", 4) != 0) {
        return SampleExtent{};
    }
    const std::uint64_t form_size = read_be32(form + 4);
    std::uint64_t form_end = 0;
    if (form_size < 4 || !checked_add_u64(8, form_size, &form_end) ||
        form_end != file_size) {
        return SampleExtent{};
    }

    bool have_comm = false;
    bool have_ssnd = false;
    std::uint16_t channels = 0;
    std::uint32_t frame_count = 0;
    std::uint16_t sample_bits = 0;
    std::uint64_t sound_bytes = 0;
    std::uint64_t offset = 12;
    std::size_t chunk_count = 0;
    constexpr std::size_t kMaximumChunks = 4096;
    while (offset + 8 <= form_end) {
        if (++chunk_count > kMaximumChunks || !seek_binary(input, offset)) {
            return SampleExtent{};
        }
        unsigned char header[8]{};
        if (!read_exact(input, header, sizeof(header))) {
            return SampleExtent{};
        }
        const std::uint64_t chunk_size = read_be32(header + 4);
        const std::uint64_t payload_offset = offset + 8;
        std::uint64_t payload_end = 0;
        if (!checked_add_u64(payload_offset, chunk_size, &payload_end)) {
            return SampleExtent{};
        }
        const std::uint64_t padding = chunk_size & 1U;
        std::uint64_t next = 0;
        if (!checked_add_u64(payload_end, padding, &next) || next > form_end) {
            return SampleExtent{};
        }

        if (std::memcmp(header, "COMM", 4) == 0) {
            if (have_comm || chunk_size < 18) {
                return SampleExtent{};
            }
            unsigned char comm[18]{};
            if (!read_exact(input, comm, sizeof(comm))) {
                return SampleExtent{};
            }
            channels = read_be16(comm);
            frame_count = read_be32(comm + 2);
            sample_bits = read_be16(comm + 6);
            have_comm = true;
        } else if (std::memcmp(header, "SSND", 4) == 0) {
            if (have_ssnd || chunk_size < 8) {
                return SampleExtent{};
            }
            unsigned char ssnd[8]{};
            if (!read_exact(input, ssnd, sizeof(ssnd))) {
                return SampleExtent{};
            }
            const std::uint64_t data_offset = read_be32(ssnd);
            if (data_offset > chunk_size - 8) {
                return SampleExtent{};
            }
            sound_bytes = chunk_size - 8 - data_offset;
            have_ssnd = true;
        }
        offset = next;
    }

    if (offset != form_end || !have_comm || !have_ssnd || frame_count == 0 ||
        channels == 0 || !(sample_bits == 8 || sample_bits == 16 ||
                           sample_bits == 24 || sample_bits == 32) ||
        static_cast<int>(channels) != facts.channels) {
        return SampleExtent{};
    }
    const std::uint64_t bytes_per_sample = sample_bits / 8U;
    std::uint64_t block_align = 0;
    std::uint64_t expected_bytes = 0;
    if (!checked_multiply_u64(channels, bytes_per_sample, &block_align) ||
        block_align == 0 ||
        !checked_multiply_u64(frame_count, block_align, &expected_bytes) ||
        expected_bytes != sound_bytes ||
        (facts.block_align > 0 &&
         static_cast<std::uint64_t>(facts.block_align) != block_align)) {
        return SampleExtent{};
    }
    const int coded_bits = facts.bits_per_coded_sample;
    if (coded_bits > 0 && coded_bits != sample_bits) {
        return SampleExtent{};
    }
    return exact_presentation_file_extent(
        frame_count, SampleExtentSource::AiffPcmData);
}

SampleExtent verify_au_pcm_extent(const std::string& path,
                                  const ContainerBoundaryFacts& facts) {
    const std::string codec_name = lower_copy(facts.codec_name);
    if (facts.sample_rate == 0 ||
        facts.channels == 0 || !fixed_linear_pcm_codec_name(codec_name)) {
        return SampleExtent{};
    }

    std::ifstream input;
    std::uint64_t file_size = 0;
    if (!open_binary_with_size(path, &input, &file_size) || file_size < 24) {
        return SampleExtent{};
    }
    unsigned char header[24]{};
    if (!read_exact(input, header, sizeof(header)) ||
        std::memcmp(header, ".snd", 4) != 0) {
        return SampleExtent{};
    }
    const std::uint64_t data_offset = read_be32(header + 4);
    const std::uint32_t data_size32 = read_be32(header + 8);
    const std::uint32_t encoding = read_be32(header + 12);
    const std::uint32_t sample_rate = read_be32(header + 16);
    const std::uint32_t channels = read_be32(header + 20);
    const char* expected_codec = au_pcm_codec_name_for_encoding(encoding);
    if (data_offset < 24 || data_size32 == 0 || data_size32 == 0xFFFFFFFFU ||
        expected_codec == nullptr || codec_name != expected_codec ||
        sample_rate != static_cast<std::uint32_t>(facts.sample_rate) ||
        channels == 0 || static_cast<int>(channels) != facts.channels) {
        return SampleExtent{};
    }
    std::uint64_t data_end = 0;
    if (!checked_add_u64(data_offset, data_size32, &data_end) || data_end != file_size) {
        return SampleExtent{};
    }
    const int bits = facts.bits_per_coded_sample;
    if (bits <= 0 || (bits % 8) != 0) {
        return SampleExtent{};
    }
    std::uint64_t block_align = 0;
    if (!checked_multiply_u64(channels, static_cast<std::uint64_t>(bits / 8),
                              &block_align) || block_align == 0 ||
        data_size32 % block_align != 0 ||
        (facts.block_align > 0 &&
         static_cast<std::uint64_t>(facts.block_align) != block_align)) {
        return SampleExtent{};
    }
    return exact_presentation_file_extent(data_size32 / block_align,
                             SampleExtentSource::AuPcmData);
}

bool caf_read_integral_sample_rate(const unsigned char* data,
                                   std::uint32_t* sample_rate) {
    if (sample_rate == nullptr) {
        return false;
    }
    const std::uint64_t bits = read_be64(data);
    double value = 0.0;
    // read_be64() already produces the numeric IEEE-754 bit pattern. Copying
    // that integer object representation into a double is endian-neutral.
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value) || value <= 0.0 ||
        value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    const double rounded = std::floor(value + 0.5);
    if (std::fabs(value - rounded) > 1.0e-9) {
        return false;
    }
    *sample_rate = static_cast<std::uint32_t>(rounded);
    return *sample_rate > 0;
}

SampleExtent verify_caf_lpcm_extent(const std::string& path,
                                    const ContainerBoundaryFacts& facts) {
    const std::string codec_name = lower_copy(facts.codec_name);
    if (facts.sample_rate == 0 ||
        facts.channels == 0 || !fixed_linear_pcm_codec_name(codec_name) ||
        facts.initial_padding != 0 || facts.trailing_padding != 0) {
        return SampleExtent{};
    }

    std::ifstream input;
    std::uint64_t file_size = 0;
    if (!open_binary_with_size(path, &input, &file_size) || file_size < 52) {
        return SampleExtent{};
    }
    unsigned char file_header[8]{};
    if (!read_exact(input, file_header, sizeof(file_header)) ||
        std::memcmp(file_header, "caff", 4) != 0 ||
        read_be16(file_header + 4) != 1) {
        return SampleExtent{};
    }

    unsigned char chunk_header[12]{};
    if (!read_exact(input, chunk_header, sizeof(chunk_header)) ||
        std::memcmp(chunk_header, "desc", 4) != 0 ||
        read_be64(chunk_header + 4) != 32) {
        return SampleExtent{};
    }
    unsigned char desc[32]{};
    if (!read_exact(input, desc, sizeof(desc)) ||
        std::memcmp(desc + 8, "lpcm", 4) != 0) {
        return SampleExtent{};
    }
    std::uint32_t sample_rate = 0;
    if (!caf_read_integral_sample_rate(desc, &sample_rate)) {
        return SampleExtent{};
    }
    const std::uint32_t format_flags = read_be32(desc + 12);
    const std::uint32_t bytes_per_packet = read_be32(desc + 16);
    const std::uint32_t frames_per_packet = read_be32(desc + 20);
    const std::uint32_t channels = read_be32(desc + 24);
    const std::uint32_t bits_per_channel = read_be32(desc + 28);
    // kAudioFormatFlagIsNonInterleaved (1 << 5) describes channel planes,
    // while this exact-data proof deliberately accepts only fixed interleaved
    // LPCM whose packet byte geometry maps directly to decoded frames.
    constexpr std::uint32_t kCafNonInterleavedFlag = 1U << 5U;
    if (sample_rate != static_cast<std::uint32_t>(facts.sample_rate) ||
        (format_flags & kCafNonInterleavedFlag) != 0 ||
        bytes_per_packet == 0 || frames_per_packet != 1 || channels == 0 ||
        static_cast<int>(channels) != facts.channels ||
        bits_per_channel == 0 || bits_per_channel > 64 ||
        bytes_per_packet % channels != 0 ||
        bits_per_channel > (bytes_per_packet / channels) * 8U ||
        (facts.block_align > 0 &&
         static_cast<std::uint32_t>(facts.block_align) != bytes_per_packet)) {
        return SampleExtent{};
    }

    bool have_data = false;
    bool have_pakt = false;
    std::uint64_t data_bytes = 0;
    std::uint64_t pakt_packets = 0;
    std::uint64_t pakt_valid_frames = 0;
    std::uint32_t pakt_priming = 0;
    std::uint32_t pakt_remainder = 0;
    std::uint64_t offset = 52;
    std::size_t chunk_count = 0;
    constexpr std::size_t kMaximumChunks = 4096;
    while (offset < file_size) {
        if (++chunk_count > kMaximumChunks || offset + 12 > file_size ||
            !seek_binary(input, offset) ||
            !read_exact(input, chunk_header, sizeof(chunk_header))) {
            return SampleExtent{};
        }
        const std::uint64_t raw_size = read_be64(chunk_header + 4);
        if ((raw_size & (1ULL << 63U)) != 0) {
            return SampleExtent{};
        }
        const std::uint64_t payload_offset = offset + 12;
        std::uint64_t payload_end = 0;
        if (!checked_add_u64(payload_offset, raw_size, &payload_end) ||
            payload_end > file_size) {
            return SampleExtent{};
        }
        if (std::memcmp(chunk_header, "data", 4) == 0) {
            if (have_data || raw_size < 4) {
                return SampleExtent{};
            }
            unsigned char edit_count[4]{};
            if (!read_exact(input, edit_count, sizeof(edit_count)) ||
                read_be32(edit_count) != 0) {
                return SampleExtent{};
            }
            data_bytes = raw_size - 4;
            have_data = true;
        } else if (std::memcmp(chunk_header, "pakt", 4) == 0) {
            if (have_pakt || raw_size < 24) {
                return SampleExtent{};
            }
            unsigned char pakt[24]{};
            if (!read_exact(input, pakt, sizeof(pakt))) {
                return SampleExtent{};
            }
            pakt_packets = read_be64(pakt);
            pakt_valid_frames = read_be64(pakt + 8);
            pakt_priming = read_be32(pakt + 16);
            pakt_remainder = read_be32(pakt + 20);
            have_pakt = true;
        }
        offset = payload_end;
    }
    if (offset != file_size || !have_data || data_bytes == 0 ||
        data_bytes % bytes_per_packet != 0) {
        return SampleExtent{};
    }
    const std::uint64_t frames = data_bytes / bytes_per_packet;
    if (frames == 0) {
        return SampleExtent{};
    }
    if (have_pakt &&
        (pakt_priming != 0 || pakt_remainder != 0 ||
         (pakt_packets != 0 && pakt_packets != frames) ||
         pakt_valid_frames != frames)) {
        return SampleExtent{};
    }
    return exact_presentation_file_extent(
        frames, SampleExtentSource::CafLpcmData);
}

SampleExtent verify_tta_extent(const std::string& path,
                               const ContainerBoundaryFacts& facts) {
    const std::string codec_name = lower_copy(facts.codec_name);
    if (codec_name != "tta" || facts.sample_rate == 0 || facts.channels == 0 ||
        facts.initial_padding != 0 || facts.trailing_padding != 0) {
        return SampleExtent{};
    }

    std::ifstream input;
    std::uint64_t file_size = 0;
    if (!open_binary_with_size(path, &input, &file_size) || file_size < 30) {
        return SampleExtent{};
    }

    unsigned char header[22]{};
    if (!read_exact(input, header, sizeof(header)) ||
        std::memcmp(header, "TTA1", 4) != 0) {
        return SampleExtent{};
    }

    const std::uint16_t format = read_le16(header + 4);
    const std::uint16_t channels = read_le16(header + 6);
    const std::uint16_t sample_bits = read_le16(header + 8);
    const std::uint32_t sample_rate = read_le32(header + 10);
    const std::uint32_t sample_count = read_le32(header + 14);
    const std::uint32_t stored_header_crc = read_le32(header + 18);

    // Keep the exact path deliberately narrow: normal unencrypted TTA1 using
    // sample widths supported by the direct FFmpeg TTA decoder.
    if (format != 1 || channels == 0 || channels > 16 ||
        !(sample_bits == 8 || sample_bits == 16 || sample_bits == 24) ||
        sample_rate == 0 || sample_rate > 1000000U || sample_count == 0 ||
        sample_rate != facts.sample_rate ||
        channels != facts.channels ||
        (facts.bits_per_coded_sample > 0 &&
         sample_bits != facts.bits_per_coded_sample) ||
        crc32_ieee_le(header, 18) != stored_header_crc) {
        return SampleExtent{};
    }

    const std::uint64_t frame_size =
        (static_cast<std::uint64_t>(sample_rate) * 256U) / 245U;
    if (frame_size == 0) {
        return SampleExtent{};
    }
    const std::uint64_t total_frames =
        (static_cast<std::uint64_t>(sample_count) + frame_size - 1U) / frame_size;
    if (total_frames == 0 || total_frames > 4000000U) {
        return SampleExtent{};
    }

    std::uint64_t table_bytes = 0;
    if (!checked_multiply_u64(total_frames, 4U, &table_bytes)) {
        return SampleExtent{};
    }
    std::uint64_t frame_data_offset = 0;
    if (!checked_add_u64(sizeof(header), table_bytes, &frame_data_offset) ||
        !checked_add_u64(frame_data_offset, 4U, &frame_data_offset) ||
        frame_data_offset > file_size) {
        return SampleExtent{};
    }

    std::uint32_t seek_crc = 0xFFFFFFFFU;
    std::uint64_t encoded_bytes = 0;
    for (std::uint64_t frame = 0; frame < total_frames; ++frame) {
        unsigned char size_data[4]{};
        if (!read_exact(input, size_data, sizeof(size_data))) {
            return SampleExtent{};
        }
        seek_crc = crc32_ieee_le_update(seek_crc, size_data, sizeof(size_data));
        const std::uint32_t encoded_frame_size = read_le32(size_data);
        if (encoded_frame_size < 4 ||
            !checked_add_u64(encoded_bytes, encoded_frame_size, &encoded_bytes)) {
            return SampleExtent{};
        }
    }
    unsigned char seek_crc_data[4]{};
    if (!read_exact(input, seek_crc_data, sizeof(seek_crc_data)) ||
        (seek_crc ^ 0xFFFFFFFFU) != read_le32(seek_crc_data)) {
        return SampleExtent{};
    }

    std::uint64_t audio_end = 0;
    if (!checked_add_u64(frame_data_offset, encoded_bytes, &audio_end) ||
        audio_end > file_size) {
        return SampleExtent{};
    }

    return exact_presentation_file_extent(
        sample_count, SampleExtentSource::TtaSampleCount);
}

SampleExtent verify_dsf_extent(const std::string& path,
                               const ContainerBoundaryFacts& facts) {
    const std::string codec_name = lower_copy(facts.codec_name);
    if (
        !(codec_name == "dsd_lsbf_planar" || codec_name == "dsd_msbf_planar") ||
        facts.sample_rate == 0 || facts.channels == 0) {
        return SampleExtent{};
    }

    std::ifstream input;
    std::uint64_t file_size = 0;
    if (!open_binary_with_size(path, &input, &file_size) || file_size < 92) {
        return SampleExtent{};
    }
    unsigned char dsd[28]{};
    if (!read_exact(input, dsd, sizeof(dsd)) ||
        std::memcmp(dsd, "DSD ", 4) != 0 || read_le64(dsd + 4) != 28 ||
        read_le64(dsd + 12) != file_size) {
        return SampleExtent{};
    }
    const std::uint64_t metadata_offset = read_le64(dsd + 20);

    unsigned char fmt[52]{};
    if (!read_exact(input, fmt, sizeof(fmt)) ||
        std::memcmp(fmt, "fmt ", 4) != 0 || read_le64(fmt + 4) != 52 ||
        read_le32(fmt + 12) != 1 || read_le32(fmt + 16) != 0) {
        return SampleExtent{};
    }
    const std::uint32_t channels = read_le32(fmt + 24);
    const std::uint32_t dsd_rate = read_le32(fmt + 28);
    const std::uint32_t bit_order = read_le32(fmt + 32);
    const std::uint64_t dsd_samples = read_le64(fmt + 36);
    const std::uint32_t block_size = read_le32(fmt + 44);
    if (channels == 0 || static_cast<int>(channels) != facts.channels ||
        dsd_rate == 0 || (dsd_rate % 8U) != 0 ||
        dsd_rate / 8U != static_cast<std::uint32_t>(facts.sample_rate) ||
        !(bit_order == 1 || bit_order == 8) || dsd_samples == 0 ||
        (dsd_samples % 8U) != 0 || block_size != 4096) {
        return SampleExtent{};
    }
    if ((bit_order == 1 && codec_name != "dsd_lsbf_planar") ||
        (bit_order == 8 && codec_name != "dsd_msbf_planar")) {
        return SampleExtent{};
    }

    unsigned char data_header[12]{};
    if (!read_exact(input, data_header, sizeof(data_header)) ||
        std::memcmp(data_header, "data", 4) != 0) {
        return SampleExtent{};
    }
    const std::uint64_t data_chunk_size = read_le64(data_header + 4);
    if (data_chunk_size < 12) {
        return SampleExtent{};
    }
    const std::uint64_t data_bytes = data_chunk_size - 12;
    const std::uint64_t audio_bytes_per_channel = dsd_samples / 8U;
    const std::uint64_t blocks_per_channel =
        (audio_bytes_per_channel + block_size - 1U) / block_size;
    std::uint64_t padded_per_channel = 0;
    std::uint64_t expected_data_bytes = 0;
    if (!checked_multiply_u64(blocks_per_channel, block_size,
                              &padded_per_channel) ||
        !checked_multiply_u64(padded_per_channel, channels,
                              &expected_data_bytes) ||
        data_bytes != expected_data_bytes) {
        return SampleExtent{};
    }
    std::uint64_t data_end = 0;
    if (!checked_add_u64(80, data_chunk_size, &data_end) || data_end > file_size) {
        return SampleExtent{};
    }
    if ((metadata_offset == 0 && data_end != file_size) ||
        (metadata_offset != 0 && metadata_offset != data_end)) {
        return SampleExtent{};
    }
    return exact_decoded_file_extent(
        audio_bytes_per_channel,
        SampleExtentSource::DsfSampleCount);
}

bool verify_dff_sound_properties(std::ifstream& input,
                                 std::uint64_t payload_offset,
                                 std::uint64_t payload_end,
                                 const ContainerBoundaryFacts& facts) {
    if ( payload_end < payload_offset + 4 ||
        !seek_binary(input, payload_offset)) {
        return false;
    }
    unsigned char property_type[4]{};
    if (!read_exact(input, property_type, sizeof(property_type)) ||
        std::memcmp(property_type, "SND ", 4) != 0) {
        return false;
    }

    bool have_channels = false;
    bool have_compression = false;
    bool have_sample_rate = false;
    std::uint64_t offset = payload_offset + 4;
    std::size_t chunk_count = 0;
    constexpr std::size_t kMaximumPropertyChunks = 4096;
    while (offset + 12 <= payload_end) {
        if (++chunk_count > kMaximumPropertyChunks || !seek_binary(input, offset)) {
            return false;
        }
        unsigned char header[12]{};
        if (!read_exact(input, header, sizeof(header))) {
            return false;
        }
        const std::uint64_t chunk_size = read_be64(header + 4);
        const std::uint64_t data_offset = offset + 12;
        std::uint64_t data_end = 0;
        if (!checked_add_u64(data_offset, chunk_size, &data_end)) {
            return false;
        }
        const std::uint64_t padding = chunk_size & 1U;
        std::uint64_t next = 0;
        if (!checked_add_u64(data_end, padding, &next) || next > payload_end) {
            return false;
        }

        if (std::memcmp(header, "CHNL", 4) == 0) {
            if (have_channels || chunk_size < 2 || !seek_binary(input, data_offset)) {
                return false;
            }
            unsigned char count_data[2]{};
            if (!read_exact(input, count_data, sizeof(count_data))) {
                return false;
            }
            const std::uint16_t channels = read_be16(count_data);
            std::uint64_t channel_codes_size = 0;
            if (channels == 0 ||
                static_cast<int>(channels) != facts.channels ||
                !checked_multiply_u64(channels, 4, &channel_codes_size) ||
                chunk_size < 2 + channel_codes_size) {
                return false;
            }
            have_channels = true;
        } else if (std::memcmp(header, "CMPR", 4) == 0) {
            if (have_compression || chunk_size < 4 || !seek_binary(input, data_offset)) {
                return false;
            }
            unsigned char compression[4]{};
            if (!read_exact(input, compression, sizeof(compression)) ||
                std::memcmp(compression, "DSD ", 4) != 0) {
                return false;
            }
            have_compression = true;
        } else if (std::memcmp(header, "FS  ", 4) == 0) {
            if (have_sample_rate || chunk_size < 4 || !seek_binary(input, data_offset)) {
                return false;
            }
            unsigned char rate_data[4]{};
            if (!read_exact(input, rate_data, sizeof(rate_data))) {
                return false;
            }
            const std::uint32_t dsd_rate = read_be32(rate_data);
            if (dsd_rate == 0 || (dsd_rate % 8U) != 0 ||
                dsd_rate / 8U != static_cast<std::uint32_t>(facts.sample_rate)) {
                return false;
            }
            have_sample_rate = true;
        }
        offset = next;
    }
    return offset == payload_end && have_channels && have_compression &&
           have_sample_rate;
}

SampleExtent verify_dff_dsd_extent(const std::string& path,
                                   const ContainerBoundaryFacts& facts) {
    const std::string codec_name = lower_copy(facts.codec_name);
    if (codec_name != "dsd_msbf" ||
        facts.sample_rate == 0 || facts.channels == 0) {
        return SampleExtent{};
    }

    std::ifstream input;
    std::uint64_t file_size = 0;
    if (!open_binary_with_size(path, &input, &file_size) || file_size < 28) {
        return SampleExtent{};
    }
    unsigned char form[16]{};
    if (!read_exact(input, form, sizeof(form)) ||
        std::memcmp(form, "FRM8", 4) != 0 ||
        std::memcmp(form + 12, "DSD ", 4) != 0) {
        return SampleExtent{};
    }
    const std::uint64_t form_size = read_be64(form + 4);
    std::uint64_t form_end = 0;
    if (form_size < 4 || !checked_add_u64(12, form_size, &form_end) ||
        form_end != file_size) {
        return SampleExtent{};
    }

    bool have_raw_dsd = false;
    bool have_prop = false;
    bool saw_diin = false;
    std::uint64_t dsd_bytes = 0;
    std::uint64_t offset = 16;
    std::size_t chunk_count = 0;
    constexpr std::size_t kMaximumChunks = 4096;
    while (offset + 12 <= form_end) {
        if (++chunk_count > kMaximumChunks || !seek_binary(input, offset)) {
            return SampleExtent{};
        }
        unsigned char header[12]{};
        if (!read_exact(input, header, sizeof(header))) {
            return SampleExtent{};
        }
        const std::uint64_t chunk_size = read_be64(header + 4);
        const std::uint64_t payload_offset = offset + 12;
        std::uint64_t payload_end = 0;
        if (!checked_add_u64(payload_offset, chunk_size, &payload_end)) {
            return SampleExtent{};
        }
        const std::uint64_t padding = chunk_size & 1U;
        std::uint64_t next = 0;
        if (!checked_add_u64(payload_end, padding, &next) || next > form_end) {
            return SampleExtent{};
        }
        if (std::memcmp(header, "DSD ", 4) == 0) {
            if (have_raw_dsd || chunk_size == 0) {
                return SampleExtent{};
            }
            dsd_bytes = chunk_size;
            have_raw_dsd = true;
        } else if (std::memcmp(header, "DST ", 4) == 0) {
            return SampleExtent{};
        } else if (std::memcmp(header, "DIIN", 4) == 0) {
            saw_diin = true;
        } else if (std::memcmp(header, "PROP", 4) == 0) {
            if (have_prop || chunk_size < 4 ||
                !verify_dff_sound_properties(input,
                                             payload_offset,
                                             payload_end,
                                             facts)) {
                return SampleExtent{};
            }
            have_prop = true;
        }
        offset = next;
    }
    const std::uint64_t channels = static_cast<std::uint64_t>(
        facts.channels);
    if (offset != form_end || !have_raw_dsd || !have_prop || saw_diin ||
        channels == 0 || dsd_bytes % channels != 0) {
        return SampleExtent{};
    }
    const std::uint64_t frames = dsd_bytes / channels;
    return exact_decoded_file_extent(
        frames,
        SampleExtentSource::DffDsdData);
}

} // namespace

SampleExtent verify_container_sample_extent(const std::string& path,
                                            const ContainerBoundaryFacts& facts) {
    if (format_name_has_token(facts.demuxer_name, "aiff")) {
        return verify_aiff_pcm_extent(path, facts);
    }
    if (format_name_has_token(facts.demuxer_name, "au")) {
        return verify_au_pcm_extent(path, facts);
    }
    if (format_name_has_token(facts.demuxer_name, "caf")) {
        return verify_caf_lpcm_extent(path, facts);
    }
    if (format_name_has_token(facts.demuxer_name, "tta")) {
        return verify_tta_extent(path, facts);
    }
    if (format_name_has_token(facts.demuxer_name, "dsf")) {
        return verify_dsf_extent(path, facts);
    }
    if (format_name_has_token(facts.demuxer_name, "iff")) {
        return verify_dff_dsd_extent(path, facts);
    }
    return SampleExtent{};
}


} // namespace pcmtp
