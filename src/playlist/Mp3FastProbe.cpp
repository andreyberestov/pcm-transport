// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/playlist/Mp3FastProbe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {
namespace {

constexpr std::uint64_t kMaximumId3TagBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumApeTagBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumFirstFrameSearchBytes = 1024ULL * 1024ULL;
constexpr std::uint32_t kMaximumApeItems = 4096U;

class FileReader final {
public:
    explicit FileReader(const std::string& path) {
        descriptor_ = ::open(path.c_str(), O_RDONLY);
        if (descriptor_ < 0) {
            return;
        }
        struct stat status {};
        if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            return;
        }
        size_ = static_cast<std::uint64_t>(status.st_size);
    }

    ~FileReader() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool valid() const noexcept {
        return descriptor_ >= 0;
    }

    std::uint64_t size() const noexcept {
        return size_;
    }

    bool read(std::uint64_t offset, void* destination, std::size_t bytes) const {
        if (!valid() || destination == nullptr || offset > size_ ||
            bytes > size_ - offset ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return false;
        }

        unsigned char* output = static_cast<unsigned char*>(destination);
        std::size_t completed = 0;
        while (completed < bytes) {
            const std::uint64_t current_offset = offset + completed;
            if (current_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                return false;
            }
            const ssize_t count = ::pread(descriptor_,
                                          output + completed,
                                          bytes - completed,
                                          static_cast<off_t>(current_offset));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return false;
            }
            completed += static_cast<std::size_t>(count);
        }
        return true;
    }

private:
    int descriptor_ = -1;
    std::uint64_t size_ = 0;
};

std::uint32_t read_be24(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 16U) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           static_cast<std::uint32_t>(bytes[2]);
}

std::uint32_t read_be32(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint32_t read_le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool read_synchsafe32(const unsigned char* bytes, std::uint32_t* value) {
    if (value == nullptr || (bytes[0] & 0x80U) != 0 || (bytes[1] & 0x80U) != 0 ||
        (bytes[2] & 0x80U) != 0 || (bytes[3] & 0x80U) != 0) {
        return false;
    }
    *value = (static_cast<std::uint32_t>(bytes[0]) << 21U) |
             (static_cast<std::uint32_t>(bytes[1]) << 14U) |
             (static_cast<std::uint32_t>(bytes[2]) << 7U) |
             static_cast<std::uint32_t>(bytes[3]);
    return true;
}

bool is_frame_identifier(const unsigned char* bytes, std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char value = bytes[index];
        if (!(value >= 'A' && value <= 'Z') && !(value >= '0' && value <= '9')) {
            return false;
        }
    }
    return true;
}

std::string trim_ascii(std::string value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}


std::string latin1_to_utf8(const unsigned char* bytes, std::size_t length) {
    std::string result;
    result.reserve(length * 2U);
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char value = bytes[index];
        if (value < 0x80U) {
            result.push_back(static_cast<char>(value));
        } else {
            result.push_back(static_cast<char>(0xC0U | (value >> 6U)));
            result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        }
    }
    return result;
}

void append_utf8_codepoint(std::uint32_t codepoint, std::string* output) {
    if (output == nullptr) {
        return;
    }
    if (codepoint <= 0x7FU) {
        output->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output->push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output->push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0x10FFFFU) {
        output->push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output->push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

bool utf16_to_utf8(const unsigned char* bytes,
                   std::size_t length,
                   bool big_endian,
                   std::string* output) {
    if (output == nullptr || (length & 1U) != 0) {
        return false;
    }
    output->clear();
    for (std::size_t offset = 0; offset + 1 < length; offset += 2) {
        const std::uint16_t first = big_endian
            ? static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                         bytes[offset + 1])
            : static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 1]) << 8U) |
                                         bytes[offset]);
        if (first == 0) {
            break;
        }
        std::uint32_t codepoint = first;
        if (first >= 0xD800U && first <= 0xDBFFU) {
            if (offset + 3 >= length) {
                return false;
            }
            const std::uint16_t second = big_endian
                ? static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 2]) << 8U) |
                                             bytes[offset + 3])
                : static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset + 3]) << 8U) |
                                             bytes[offset + 2]);
            if (second < 0xDC00U || second > 0xDFFFU) {
                return false;
            }
            codepoint = 0x10000U +
                        ((static_cast<std::uint32_t>(first) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(second) - 0xDC00U);
            offset += 2;
        } else if (first >= 0xDC00U && first <= 0xDFFFU) {
            return false;
        }
        append_utf8_codepoint(codepoint, output);
    }
    return true;
}

bool decode_id3_text(const unsigned char* bytes,
                     std::size_t length,
                     std::string* output) {
    if (output == nullptr || bytes == nullptr || length < 1) {
        return false;
    }

    const unsigned char encoding = bytes[0];
    const unsigned char* text = bytes + 1;
    std::size_t text_length = length - 1;
    std::string decoded;

    if (encoding == 0U || encoding == 3U) {
        const unsigned char* nul = static_cast<const unsigned char*>(
            std::memchr(text, 0, text_length));
        if (nul != nullptr) {
            text_length = static_cast<std::size_t>(nul - text);
        }
        if (encoding == 0U) {
            decoded = latin1_to_utf8(text, text_length);
        } else {
            decoded.assign(reinterpret_cast<const char*>(text), text_length);
        }
    } else if (encoding == 1U) {
        if (text_length < 2) {
            return false;
        }
        bool big_endian = false;
        if (text[0] == 0xFEU && text[1] == 0xFFU) {
            big_endian = true;
        } else if (text[0] == 0xFFU && text[1] == 0xFEU) {
            big_endian = false;
        } else {
            return false;
        }
        text += 2;
        text_length -= 2;
        if ((text_length & 1U) != 0) {
            --text_length;
        }
        if (!utf16_to_utf8(text, text_length, big_endian, &decoded)) {
            return false;
        }
    } else if (encoding == 2U) {
        if ((text_length & 1U) != 0) {
            --text_length;
        }
        if (!utf16_to_utf8(text, text_length, true, &decoded)) {
            return false;
        }
    } else {
        return false;
    }

    decoded = trim_ascii(text::normalize_metadata_value(decoded));
    *output = std::move(decoded);
    return true;
}

int parse_track_number(const std::string& value) {
    std::size_t offset = 0;
    while (offset < value.size() &&
           std::isspace(static_cast<unsigned char>(value[offset])) != 0) {
        ++offset;
    }
    int track = 0;
    bool have_digit = false;
    while (offset < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[offset])) != 0) {
        have_digit = true;
        const int digit = value[offset] - '0';
        if (track > (std::numeric_limits<int>::max() - digit) / 10) {
            return 0;
        }
        track = track * 10 + digit;
        ++offset;
    }
    return have_digit ? track : 0;
}

void apply_tag_value(const std::string& identifier,
                     const std::string& value,
                     GenericTags* tags) {
    if (tags == nullptr || value.empty()) {
        return;
    }
    if ((identifier == "TIT2" || identifier == "TT2" || identifier == "TITLE") &&
        tags->title.empty()) {
        tags->title = value;
    } else if ((identifier == "TPE1" || identifier == "TP1" || identifier == "ARTIST") &&
               tags->artist.empty()) {
        tags->artist = value;
    } else if ((identifier == "TALB" || identifier == "TAL" || identifier == "ALBUM") &&
               tags->album.empty()) {
        tags->album = value;
    } else if ((identifier == "TRCK" || identifier == "TRK" || identifier == "TRACK" ||
                identifier == "TRACKNUMBER") && tags->track_number == 0) {
        tags->track_number = parse_track_number(value);
    }
}

bool is_relevant_text_frame(const std::string& identifier) {
    return identifier == "TIT2" || identifier == "TT2" ||
           identifier == "TPE1" || identifier == "TP1" ||
           identifier == "TALB" || identifier == "TAL" ||
           identifier == "TRCK" || identifier == "TRK";
}

struct TagParseResult {
    bool ok = true;
    std::string diagnostic;
    std::uint64_t audio_start = 0;
    std::uint64_t audio_end = 0;
    GenericTags tags{};
};

bool parse_id3v2(const FileReader& file, TagParseResult* result) {
    if (result == nullptr || file.size() < 10) {
        return true;
    }

    std::array<unsigned char, 10> header{};
    if (!file.read(0, header.data(), header.size())) {
        result->ok = false;
        result->diagnostic = "cannot read ID3 header";
        return false;
    }
    if (std::memcmp(header.data(), "ID3", 3) != 0) {
        return true;
    }

    const unsigned int version = header[3];
    const unsigned char flags = header[5];
    if (version < 2U || version > 4U || header[4] == 0xFFU) {
        result->ok = false;
        result->diagnostic = "unsupported ID3v2 version";
        return false;
    }
    if (version == 2U && (flags & 0x40U) != 0) {
        result->ok = false;
        result->diagnostic = "compressed ID3v2.2 tag";
        return false;
    }

    std::uint32_t tag_size = 0;
    if (!read_synchsafe32(header.data() + 6, &tag_size) ||
        tag_size > kMaximumId3TagBytes ||
        static_cast<std::uint64_t>(tag_size) + 10ULL > file.size()) {
        result->ok = false;
        result->diagnostic = "invalid or oversized ID3v2 tag";
        return false;
    }

    std::vector<unsigned char> body(tag_size);
    if (tag_size > 0 && !file.read(10, body.data(), body.size())) {
        result->ok = false;
        result->diagnostic = "cannot read ID3v2 tag";
        return false;
    }

    const bool tag_unsynchronized = (flags & 0x80U) != 0;
    if (tag_unsynchronized) {
        result->ok = false;
        result->diagnostic = "unsynchronized ID3v2 tag";
        return false;
    }
    std::size_t offset = 0;
    if ((flags & 0x40U) != 0 && version >= 3U) {
        if (body.size() < 4) {
            result->ok = false;
            result->diagnostic = "truncated ID3 extended header";
            return false;
        }
        std::uint32_t extended_size = 0;
        if (version == 3U) {
            extended_size = read_be32(body.data());
            if (extended_size > body.size() - 4U) {
                result->ok = false;
                result->diagnostic = "invalid ID3v2.3 extended header";
                return false;
            }
            offset = 4U + extended_size;
        } else {
            if (!read_synchsafe32(body.data(), &extended_size) ||
                extended_size < 4U || extended_size > body.size()) {
                result->ok = false;
                result->diagnostic = "invalid ID3v2.4 extended header";
                return false;
            }
            offset = extended_size;
        }
    }

    while (offset < body.size()) {
        const std::size_t header_size = version == 2U ? 6U : 10U;
        if (body.size() - offset < header_size) {
            break;
        }
        const std::size_t identifier_size = version == 2U ? 3U : 4U;
        const unsigned char* frame_header = body.data() + offset;
        bool padding = true;
        for (std::size_t index = 0; index < identifier_size; ++index) {
            if (frame_header[index] != 0) {
                padding = false;
                break;
            }
        }
        if (padding) {
            break;
        }
        if (!is_frame_identifier(frame_header, identifier_size)) {
            result->ok = false;
            result->diagnostic = "invalid ID3 frame identifier";
            return false;
        }

        const std::string identifier(reinterpret_cast<const char*>(frame_header),
                                     identifier_size);
        std::uint32_t frame_size = 0;
        std::uint16_t frame_flags = 0;
        if (version == 2U) {
            frame_size = read_be24(frame_header + 3);
        } else if (version == 3U) {
            frame_size = read_be32(frame_header + 4);
            frame_flags = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(frame_header[8]) << 8U) | frame_header[9]);
        } else {
            if (!read_synchsafe32(frame_header + 4, &frame_size)) {
                result->ok = false;
                result->diagnostic = "invalid ID3v2.4 frame size";
                return false;
            }
            frame_flags = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(frame_header[8]) << 8U) | frame_header[9]);
        }
        offset += header_size;
        if (frame_size > body.size() - offset) {
            result->ok = false;
            result->diagnostic = "truncated ID3 frame";
            return false;
        }

        if (is_relevant_text_frame(identifier)) {
            const bool compressed = version == 3U
                ? (frame_flags & 0x0080U) != 0
                : version == 4U && (frame_flags & 0x0008U) != 0;
            const bool encrypted = version == 3U
                ? (frame_flags & 0x0040U) != 0
                : version == 4U && (frame_flags & 0x0004U) != 0;
            if (compressed || encrypted) {
                result->ok = false;
                result->diagnostic = "compressed or encrypted metadata frame";
                return false;
            }

            const unsigned char* frame_data = body.data() + offset;
            std::size_t frame_length = frame_size;
            std::size_t prefix = 0;
            if (version == 3U && (frame_flags & 0x0020U) != 0) {
                prefix = 1;
            } else if (version == 4U) {
                if ((frame_flags & 0x0040U) != 0) {
                    ++prefix;
                }
                if ((frame_flags & 0x0001U) != 0) {
                    prefix += 4;
                }
            }
            if (prefix > frame_length) {
                result->ok = false;
                result->diagnostic = "invalid ID3 frame prefix";
                return false;
            }
            frame_data += prefix;
            frame_length -= prefix;

            const bool frame_unsynchronized = version == 4U &&
                                               (frame_flags & 0x0002U) != 0;
            if (frame_unsynchronized) {
                result->ok = false;
                result->diagnostic = "unsynchronized ID3v2.4 metadata frame";
                return false;
            }

            std::string value;
            if (!decode_id3_text(frame_data, frame_length, &value)) {
                result->ok = false;
                result->diagnostic = "unsupported ID3 text encoding";
                return false;
            }
            apply_tag_value(identifier, value, &result->tags);
        }
        offset += frame_size;
    }

    result->audio_start = 10ULL + tag_size;
    if (version == 4U && (flags & 0x10U) != 0) {
        if (result->audio_start + 10ULL > file.size()) {
            result->ok = false;
            result->diagnostic = "truncated ID3v2.4 footer";
            return false;
        }
        std::array<unsigned char, 10> footer{};
        if (!file.read(result->audio_start, footer.data(), footer.size()) ||
            std::memcmp(footer.data(), "3DI", 3) != 0 ||
            footer[3] != header[3] || footer[4] != header[4]) {
            result->ok = false;
            result->diagnostic = "invalid ID3v2.4 footer";
            return false;
        }
        result->audio_start += 10ULL;
    }
    return true;
}

bool parse_id3v1(const FileReader& file, TagParseResult* result) {
    if (result == nullptr || result->audio_end < 128ULL) {
        return true;
    }
    std::array<unsigned char, 128> tag{};
    if (!file.read(result->audio_end - tag.size(), tag.data(), tag.size())) {
        return true;
    }
    if (std::memcmp(tag.data(), "TAG", 3) != 0) {
        return true;
    }

    const auto field = [&tag](std::size_t offset, std::size_t length) {
        std::size_t actual = length;
        while (actual > 0 && (tag[offset + actual - 1] == 0 || tag[offset + actual - 1] == ' ')) {
            --actual;
        }
        return trim_ascii(text::normalize_metadata_value(
            latin1_to_utf8(tag.data() + offset, actual)));
    };
    apply_tag_value("TITLE", field(3, 30), &result->tags);
    apply_tag_value("ARTIST", field(33, 30), &result->tags);
    apply_tag_value("ALBUM", field(63, 30), &result->tags);
    if (result->tags.track_number == 0 && tag[125] == 0 && tag[126] != 0) {
        result->tags.track_number = tag[126];
    }
    result->audio_end -= tag.size();
    return true;
}

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool parse_apev2(const FileReader& file, TagParseResult* result) {
    if (result == nullptr || result->audio_end < 32ULL) {
        return true;
    }
    std::array<unsigned char, 32> footer{};
    const std::uint64_t footer_offset = result->audio_end - footer.size();
    if (!file.read(footer_offset, footer.data(), footer.size()) ||
        std::memcmp(footer.data(), "APETAGEX", 8) != 0) {
        return true;
    }

    const std::uint32_t version = read_le32(footer.data() + 8);
    const std::uint32_t tag_size = read_le32(footer.data() + 12);
    const std::uint32_t item_count = read_le32(footer.data() + 16);
    const std::uint32_t footer_flags = read_le32(footer.data() + 20);
    if ((version != 1000U && version != 2000U) || tag_size < 32U ||
        tag_size > kMaximumApeTagBytes || item_count > kMaximumApeItems ||
        tag_size > result->audio_end) {
        result->ok = false;
        result->diagnostic = "invalid or oversized APEv2 tag";
        return false;
    }

    const std::uint64_t item_start = result->audio_end - tag_size;
    std::uint64_t full_tag_start = item_start;
    if ((footer_flags & 0x80000000U) != 0) {
        if (item_start < 32ULL) {
            result->ok = false;
            result->diagnostic = "truncated APEv2 header";
            return false;
        }
        std::array<unsigned char, 32> header{};
        if (!file.read(item_start - 32ULL, header.data(), header.size()) ||
            std::memcmp(header.data(), "APETAGEX", 8) != 0) {
            result->ok = false;
            result->diagnostic = "missing APEv2 header";
            return false;
        }
        full_tag_start = item_start - 32ULL;
    }

    const std::size_t payload_size = static_cast<std::size_t>(tag_size - 32U);
    std::vector<unsigned char> payload(payload_size);
    if (payload_size > 0 && !file.read(item_start, payload.data(), payload.size())) {
        result->ok = false;
        result->diagnostic = "cannot read APEv2 tag";
        return false;
    }

    std::size_t offset = 0;
    for (std::uint32_t item = 0; item < item_count; ++item) {
        if (payload.size() - offset < 8U) {
            result->ok = false;
            result->diagnostic = "truncated APEv2 item";
            return false;
        }
        const std::uint32_t value_size = read_le32(payload.data() + offset);
        const std::uint32_t flags = read_le32(payload.data() + offset + 4);
        offset += 8;

        const auto key_end = std::find(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                       payload.end(), 0U);
        if (key_end == payload.end()) {
            result->ok = false;
            result->diagnostic = "unterminated APEv2 key";
            return false;
        }
        const std::size_t key_end_offset = static_cast<std::size_t>(
            std::distance(payload.begin(), key_end));
        const std::string key(reinterpret_cast<const char*>(payload.data() + offset),
                              key_end_offset - offset);
        offset = key_end_offset + 1;
        if (value_size > payload.size() - offset) {
            result->ok = false;
            result->diagnostic = "truncated APEv2 value";
            return false;
        }

        const std::string upper_key = uppercase_ascii(key);
        const bool relevant = upper_key == "TITLE" || upper_key == "ARTIST" ||
                              upper_key == "ALBUM" || upper_key == "TRACK" ||
                              upper_key == "TRACKNUMBER";
        const std::uint32_t value_type = (flags >> 1U) & 0x3U;
        if (relevant) {
            if (value_type != 0U) {
                result->ok = false;
                result->diagnostic = "non-text APEv2 metadata item";
                return false;
            }
            std::size_t text_length = value_size;
            const unsigned char* value = payload.data() + offset;
            const unsigned char* nul = static_cast<const unsigned char*>(
                std::memchr(value, 0, text_length));
            if (nul != nullptr) {
                text_length = static_cast<std::size_t>(nul - value);
            }
            const std::string normalized = trim_ascii(text::normalize_metadata_value(
                std::string(reinterpret_cast<const char*>(value), text_length)));
            apply_tag_value(upper_key, normalized, &result->tags);
        }
        offset += value_size;
    }

    result->audio_end = full_tag_start;
    return true;
}

struct MpegFrameHeader {
    unsigned int version = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t bitrate_kbps = 0;
    std::uint32_t frame_size = 0;
    std::uint32_t samples_per_frame = 0;
    std::uint16_t channels = 0;
    bool has_crc = false;
};

bool parse_mpeg_layer3_header(const unsigned char* bytes, MpegFrameHeader* header) {
    if (bytes == nullptr || header == nullptr || bytes[0] != 0xFFU ||
        (bytes[1] & 0xE0U) != 0xE0U) {
        return false;
    }

    const unsigned int version_bits = (bytes[1] >> 3U) & 0x3U;
    const unsigned int layer_bits = (bytes[1] >> 1U) & 0x3U;
    if (version_bits == 1U || layer_bits != 1U) {
        return false;
    }
    const unsigned int bitrate_index = (bytes[2] >> 4U) & 0xFU;
    const unsigned int sample_rate_index = (bytes[2] >> 2U) & 0x3U;
    if (bitrate_index == 0U || bitrate_index == 0xFU || sample_rate_index == 0x3U) {
        return false;
    }

    static constexpr std::array<std::uint32_t, 16> kMpeg1Layer3Bitrates = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static constexpr std::array<std::uint32_t, 16> kMpeg2Layer3Bitrates = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static constexpr std::array<std::uint32_t, 3> kMpeg1SampleRates = {44100, 48000, 32000};
    static constexpr std::array<std::uint32_t, 3> kMpeg2SampleRates = {22050, 24000, 16000};
    static constexpr std::array<std::uint32_t, 3> kMpeg25SampleRates = {11025, 12000, 8000};

    const bool mpeg1 = version_bits == 3U;
    header->version = mpeg1 ? 1U : (version_bits == 2U ? 2U : 25U);
    header->bitrate_kbps = mpeg1
        ? kMpeg1Layer3Bitrates[bitrate_index]
        : kMpeg2Layer3Bitrates[bitrate_index];
    header->sample_rate = mpeg1
        ? kMpeg1SampleRates[sample_rate_index]
        : (version_bits == 2U
               ? kMpeg2SampleRates[sample_rate_index]
               : kMpeg25SampleRates[sample_rate_index]);
    header->samples_per_frame = mpeg1 ? 1152U : 576U;
    const bool padding = (bytes[2] & 0x02U) != 0;
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(mpeg1 ? 144000U : 72000U) * header->bitrate_kbps;
    header->frame_size = static_cast<std::uint32_t>(numerator / header->sample_rate) +
                         (padding ? 1U : 0U);
    header->channels = ((bytes[3] >> 6U) & 0x3U) == 3U ? 1U : 2U;
    header->has_crc = (bytes[1] & 0x01U) == 0;
    return header->frame_size >= 24U;
}

bool compatible_headers(const MpegFrameHeader& left, const MpegFrameHeader& right) {
    return left.version == right.version &&
           left.sample_rate == right.sample_rate &&
           left.channels == right.channels &&
           left.samples_per_frame == right.samples_per_frame;
}

bool find_first_mpeg_frame(const FileReader& file,
                           std::uint64_t audio_start,
                           std::uint64_t audio_end,
                           std::uint64_t* frame_offset,
                           MpegFrameHeader* frame_header) {
    if (frame_offset == nullptr || frame_header == nullptr || audio_start >= audio_end ||
        audio_end - audio_start < 8ULL) {
        return false;
    }

    const std::uint64_t search_size64 = std::min<std::uint64_t>(
        audio_end - audio_start, kMaximumFirstFrameSearchBytes);
    std::vector<unsigned char> data(static_cast<std::size_t>(search_size64));
    if (!file.read(audio_start, data.data(), data.size())) {
        return false;
    }

    for (std::size_t offset = 0; offset + 8U <= data.size(); ++offset) {
        MpegFrameHeader candidate;
        if (!parse_mpeg_layer3_header(data.data() + offset, &candidate)) {
            continue;
        }
        const std::uint64_t absolute = audio_start + offset;
        const std::uint64_t next_offset = absolute + candidate.frame_size;
        if (next_offset + 4ULL > audio_end) {
            continue;
        }
        std::array<unsigned char, 4> next_bytes{};
        if (!file.read(next_offset, next_bytes.data(), next_bytes.size())) {
            continue;
        }
        MpegFrameHeader next;
        if (!parse_mpeg_layer3_header(next_bytes.data(), &next) ||
            !compatible_headers(candidate, next)) {
            continue;
        }
        *frame_offset = absolute;
        *frame_header = candidate;
        return true;
    }
    return false;
}

bool has_lame_compatible_encoder_tag(const unsigned char* bytes, std::size_t length) {
    if (bytes == nullptr || length < 9) {
        return false;
    }
    return std::memcmp(bytes, "LAME", 4) == 0 ||
           std::memcmp(bytes, "Lavc", 4) == 0;
}

bool parse_xing_gapless_duration(const FileReader& file,
                                 std::uint64_t frame_offset,
                                 const MpegFrameHeader& header,
                                 std::uint64_t audio_end,
                                 std::uint64_t* decoded_samples,
                                 std::string* diagnostic) {
    if (decoded_samples == nullptr || diagnostic == nullptr ||
        frame_offset + header.frame_size > audio_end) {
        return false;
    }
    std::vector<unsigned char> frame(header.frame_size);
    if (!file.read(frame_offset, frame.data(), frame.size())) {
        *diagnostic = "cannot read first MPEG frame";
        return false;
    }

    const std::size_t side_information = header.version == 1U
        ? (header.channels == 1U ? 17U : 32U)
        : (header.channels == 1U ? 9U : 17U);
    const std::size_t xing_offset = 4U + (header.has_crc ? 2U : 0U) + side_information;
    if (xing_offset + 8U > frame.size()) {
        *diagnostic = "truncated Xing header";
        return false;
    }
    const unsigned char* xing = frame.data() + xing_offset;
    if (std::memcmp(xing, "Xing", 4) != 0 && std::memcmp(xing, "Info", 4) != 0) {
        *diagnostic = "no Xing/Info frame count";
        return false;
    }

    const std::uint32_t flags = read_be32(xing + 4);
    std::size_t offset = xing_offset + 8U;
    if ((flags & 0x1U) == 0 || offset + 4U > frame.size()) {
        *diagnostic = "Xing header has no frame count";
        return false;
    }
    const std::uint32_t frame_count = read_be32(frame.data() + offset);
    offset += 4U;
    if ((flags & 0x2U) != 0) {
        if (offset + 4U > frame.size()) {
            *diagnostic = "truncated Xing byte count";
            return false;
        }
        const std::uint32_t encoded_bytes = read_be32(frame.data() + offset);
        if (encoded_bytes == 0 || encoded_bytes > audio_end - frame_offset + 4096ULL) {
            *diagnostic = "invalid Xing byte count";
            return false;
        }
        offset += 4U;
    }
    if ((flags & 0x4U) != 0) {
        if (offset + 100U > frame.size()) {
            *diagnostic = "truncated Xing TOC";
            return false;
        }
        offset += 100U;
    }
    if ((flags & 0x8U) != 0) {
        if (offset + 4U > frame.size()) {
            *diagnostic = "truncated Xing quality field";
            return false;
        }
        offset += 4U;
    }
    if (frame_count == 0 || offset + 24U > frame.size() ||
        !has_lame_compatible_encoder_tag(frame.data() + offset, frame.size() - offset)) {
        *diagnostic = "Xing stream has no reliable encoder delay/padding";
        return false;
    }

    const unsigned char* gapless = frame.data() + offset + 21U;
    const std::uint32_t encoder_delay =
        (static_cast<std::uint32_t>(gapless[0]) << 4U) |
        (static_cast<std::uint32_t>(gapless[1]) >> 4U);
    const std::uint32_t end_padding =
        ((static_cast<std::uint32_t>(gapless[1]) & 0x0FU) << 8U) |
        static_cast<std::uint32_t>(gapless[2]);
    const std::uint64_t encoded_samples =
        static_cast<std::uint64_t>(frame_count) * header.samples_per_frame;
    if (encoded_samples == 0 ||
        static_cast<std::uint64_t>(encoder_delay) + end_padding >= encoded_samples) {
        *diagnostic = "invalid MPEG encoder delay/padding";
        return false;
    }

    *decoded_samples = encoded_samples - encoder_delay - end_padding;
    return true;
}

Mp3FastProbeResult fallback_result(std::string diagnostic,
                                   Mp3FastProbeStatus status = Mp3FastProbeStatus::NeedExternalFallback) {
    Mp3FastProbeResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    return result;
}

} // namespace

Mp3FastProbeResult probe_mp3_fast(const std::string& path) {
    FileReader file(path);
    if (!file.valid()) {
        return fallback_result("cannot open MP3", Mp3FastProbeStatus::Invalid);
    }
    if (file.size() < 8ULL) {
        return fallback_result("MP3 is too small", Mp3FastProbeStatus::Invalid);
    }

    TagParseResult tags;
    tags.audio_end = file.size();
    if (!parse_id3v2(file, &tags) || !tags.ok) {
        return fallback_result(tags.diagnostic);
    }
    if (!parse_id3v1(file, &tags) || !tags.ok) {
        return fallback_result(tags.diagnostic);
    }
    if (!parse_apev2(file, &tags) || !tags.ok) {
        return fallback_result(tags.diagnostic);
    }
    if (tags.audio_start >= tags.audio_end) {
        return fallback_result("MP3 has no audio payload", Mp3FastProbeStatus::Invalid);
    }

    std::uint64_t first_frame_offset = 0;
    MpegFrameHeader first_header;
    if (!find_first_mpeg_frame(file,
                               tags.audio_start,
                               tags.audio_end,
                               &first_frame_offset,
                               &first_header)) {
        return fallback_result("no reliable MPEG Layer III frame sequence");
    }

    std::uint64_t decoded_samples = 0;
    std::string duration_diagnostic;
    if (!parse_xing_gapless_duration(file,
                                     first_frame_offset,
                                     first_header,
                                     tags.audio_end,
                                     &decoded_samples,
                                     &duration_diagnostic)) {
        return fallback_result(duration_diagnostic);
    }

    Mp3FastProbeResult result;
    result.status = Mp3FastProbeStatus::Complete;
    result.info.format.sample_rate = first_header.sample_rate;
    result.info.format.channels = first_header.channels;
    result.info.format.bits_per_sample = 16;
    result.info.source_format = result.info.format;
    result.info.total_samples_per_channel = decoded_samples;
    result.info.source_total_samples_per_channel = decoded_samples;
    result.info.tags = std::move(tags.tags);
    result.info.codec_name = "mp3";
    result.info.lossless = false;
    result.info.sample_extent_kind = SampleExtentKind::ExactPresentationSpan;
    result.info.sample_extent_source = SampleExtentSource::CodecGaplessMetadata;
    result.info.source_sample_extent_kind = result.info.sample_extent_kind;
    result.info.source_sample_extent_source = result.info.sample_extent_source;
    result.info.probe_backend = "mp3-fast";
    return result;
}

} // namespace pcmtp
