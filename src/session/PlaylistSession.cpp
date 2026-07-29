#include "pcmtp/session/PlaylistSession.hpp"

#include "pcmtp/session/SessionLimits.hpp"

#include <glib.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

namespace pcmtp {

namespace {

constexpr std::uint16_t kMaxChannels = 32;
constexpr std::uint16_t kMaxBitsPerSample = 32;

bool fits_int(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max());
}

std::string config_directory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::string(home) + "/.config/pcm_transport";
}

bool fits_uint16(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max());
}

bool fits_uint32(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

bool fits_size(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

bool utf8_valid(const std::string& value) {
    return value.empty() ||
           g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr);
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

void append_json_string(std::ostringstream& out, const std::string& key, const std::string& value) {
    out << '"' << key << "\":\"" << json_escape(value) << '"';
}

void append_json_uint64(std::ostringstream& out, const std::string& key, std::uint64_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_uint32(std::ostringstream& out, const std::string& key, std::uint32_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_uint16(std::ostringstream& out, const std::string& key, std::uint16_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_int(std::ostringstream& out, const std::string& key, int value) {
    out << '"' << key << "\":" << value;
}

void append_json_int64(std::ostringstream& out, const std::string& key, std::int64_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_bool(std::ostringstream& out, const std::string& key, bool value) {
    out << '"' << key << "\":" << (value ? "true" : "false");
}

std::string serialize_track(const PlaylistSessionTrack& track) {
    std::ostringstream out;
    out << '{';
    append_json_string(out, "audio_file_path", track.audio_file_path);
    out << ',';
    append_json_string(out, "top_level_source_path", track.top_level_source_path);
    out << ',';
    append_json_int(out, "track_number", track.track_number);
    out << ',';
    append_json_string(out, "title", track.title);
    out << ',';
    append_json_string(out, "performer", track.performer);
    out << ',';
    append_json_uint64(out, "start_sample", track.start_sample);
    out << ',';
    append_json_uint64(out, "end_sample", track.end_sample);
    out << ',';
    append_json_uint64(out, "source_start_sample", track.source_start_sample);
    out << ',';
    append_json_uint64(out, "source_end_sample", track.source_end_sample);
    out << ',';
    append_json_uint64(out, "cue_start_frame_75", track.cue_start_frame_75);
    out << ',';
    append_json_uint64(out, "cue_end_frame_75", track.cue_end_frame_75);
    out << ',';
    append_json_bool(out, "cue_has_end_frame_75", track.cue_has_end_frame_75);
    out << ',';
    append_json_string(out, "source_label", track.source_label);
    out << ',';
    append_json_uint32(out, "decoded_sample_rate", track.decoded_sample_rate);
    out << ',';
    append_json_uint16(out, "decoded_channels", track.decoded_channels);
    out << ',';
    append_json_uint16(out, "decoded_bits_per_sample", track.decoded_bits_per_sample);
    out << ',';
    append_json_uint32(out, "source_sample_rate", track.source_sample_rate);
    out << ',';
    append_json_uint16(out, "source_bits_per_sample", track.source_bits_per_sample);
    out << ',';
    append_json_bool(out, "native_source_available", track.native_source_available);
    out << ',';
    append_json_bool(out, "native_decode", track.native_decode);
    out << ',';
    append_json_bool(out, "lossless_source", track.lossless_source);
    out << ',';
    append_json_bool(out, "lossy_source", track.lossy_source);
    out << ',';
    append_json_bool(out, "resampled", track.resampled);
    out << ',';
    append_json_uint32(out, "resampled_from_rate", track.resampled_from_rate);
    out << ',';
    append_json_bool(out, "bitdepth_converted", track.bitdepth_converted);
    out << ',';
    append_json_bool(out, "processed_by_ffmpeg", track.processed_by_ffmpeg);
    out << ',';
    append_json_string(out, "codec_name", track.codec_name);
    out << ',';
    append_json_bool(out, "dsd_source", track.dsd_source);
    out << ',';
    append_json_uint32(out, "dsd_sample_rate", track.dsd_sample_rate);
    out << ',';
    append_json_bool(out, "cue_track", track.cue_track);
    out << ',';
    append_json_uint64(out, "cue_album_end_sample", track.cue_album_end_sample);
    out << ',';
    append_json_uint64(out, "source_cue_album_end_sample", track.source_cue_album_end_sample);
    out << ',';
    append_json_uint64(out, "file_dev", track.file_dev);
    out << ',';
    append_json_uint64(out, "file_ino", track.file_ino);
    out << ',';
    append_json_uint64(out, "file_size", track.file_size);
    out << ',';
    append_json_int64(out, "file_mtime", track.file_mtime);
    out << ',';
    append_json_uint64(out, "top_level_dev", track.top_level_dev);
    out << ',';
    append_json_uint64(out, "top_level_ino", track.top_level_ino);
    out << ',';
    append_json_uint64(out, "top_level_size", track.top_level_size);
    out << ',';
    append_json_int64(out, "top_level_mtime", track.top_level_mtime);
    out << ',';
    append_json_int(out, "metadata_state", track.metadata_state);
    out << ',';
    append_json_bool(out, "is_stream", track.is_stream);
    out << '}';
    return out.str();
}

bool validate_track(const PlaylistSessionTrack& track) {
    if (track.audio_file_path.empty() || !utf8_valid(track.audio_file_path) || !utf8_valid(track.top_level_source_path) ||
        !utf8_valid(track.title) || !utf8_valid(track.performer) || !utf8_valid(track.source_label) ||
        !utf8_valid(track.codec_name)) {
        return false;
    }
    if (track.metadata_state < 0 || track.metadata_state > 2) {
        return false;
    }
    if (track.metadata_state != 1) {
        return true;
    }
    if (track.decoded_channels == 0 || track.decoded_channels > kMaxChannels) {
        return false;
    }
    if (track.decoded_bits_per_sample == 0 || track.decoded_bits_per_sample > kMaxBitsPerSample) {
        return false;
    }
    if (track.decoded_sample_rate == 0 || track.decoded_sample_rate > kMaxPcmSampleRate ||
        !is_supported_pcm_sample_rate(track.decoded_sample_rate)) {
        return false;
    }
    if (track.source_sample_rate > kMaxPcmSampleRate ||
        track.resampled_from_rate > kMaxPcmSampleRate) {
        return false;
    }
    if (track.resampled_from_rate > 0 &&
        !is_supported_pcm_sample_rate(track.resampled_from_rate)) {
        return false;
    }
    if (track.dsd_sample_rate > kMaxDsdSampleRate) {
        return false;
    }
    if (track.dsd_source) {
        if (track.dsd_sample_rate == 0 || !is_supported_dsd_sample_rate(track.dsd_sample_rate)) {
            return false;
        }
    } else if (track.dsd_sample_rate != 0) {
        return false;
    }
    if (track.native_decode && !track.native_source_available) {
        return false;
    }
    if (track.source_bits_per_sample > kMaxBitsPerSample) {
        return false;
    }
    if (track.end_sample > 0 && track.end_sample < track.start_sample) {
        return false;
    }
    if (track.source_end_sample > 0 && track.source_end_sample < track.source_start_sample) {
        return false;
    }
    if (track.cue_track) {
        if (track.source_end_sample <= track.source_start_sample) {
            return false;
        }
        if (track.cue_has_end_frame_75 && track.cue_end_frame_75 < track.cue_start_frame_75) {
            return false;
        }
    }
    return true;
}

bool validate_snapshot(const PlaylistSessionSnapshot& snapshot) {
    if (snapshot.tracks.empty() || snapshot.tracks.size() > PlaylistSessionSnapshot::kMaxTrackCount) {
        return false;
    }
    if (!fits_size(snapshot.current_track_index) || snapshot.current_track_index >= snapshot.tracks.size()) {
        return false;
    }
    for (const PlaylistSessionTrack& track : snapshot.tracks) {
        if (!validate_track(track)) {
            return false;
        }
    }
    return true;
}

class JsonReader {
public:
    explicit JsonReader(std::string text) : text_(std::move(text)) {}

    bool fully_consumed() const {
        std::size_t pos = pos_;
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos]))) {
            ++pos;
        }
        return pos == text_.size();
    }

    void skip_whitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    bool parse_string(std::string& out) {
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') {
                return utf8_valid(out);
            }
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (!std::isxdigit(static_cast<unsigned char>(text_[pos_ + static_cast<std::size_t>(i)]))) {
                            return false;
                        }
                    }
                    out += '?';
                    pos_ += 4;
                    break;
                default:
                    return false;
            }
        }
        return false;
    }

    bool parse_number(std::uint64_t& out) {
        skip_whitespace();
        const std::size_t begin = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            return false;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (begin == pos_) {
            return false;
        }
        try {
            const unsigned long long value = std::stoull(text_.substr(begin, pos_ - begin));
            out = static_cast<std::uint64_t>(value);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_bool(bool& out) {
        skip_whitespace();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool parse_key(std::string& key) {
        if (!parse_string(key)) {
            return false;
        }
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != ':') {
            return false;
        }
        ++pos_;
        return true;
    }

    bool parse_track(PlaylistSessionTrack& track, int format_version) {
        if (!consume('{')) {
            return false;
        }
        bool first = true;
        while (true) {
            skip_whitespace();
            if (consume('}')) {
                return validate_track(track);
            }
            if (!first && !consume(',')) {
                return false;
            }
            first = false;

            std::string key;
            if (!parse_key(key)) {
                return false;
            }

            if (key == "audio_file_path") {
                if (!parse_string(track.audio_file_path)) return false;
            } else if (key == "top_level_source_path") {
                if (!parse_string(track.top_level_source_path)) return false;
            } else if (key == "track_number") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_int(value)) {
                    return false;
                }
                track.track_number = static_cast<int>(value);
            } else if (key == "title") {
                if (!parse_string(track.title)) return false;
            } else if (key == "performer") {
                if (!parse_string(track.performer)) return false;
            } else if (key == "start_sample") {
                if (!parse_number(track.start_sample)) return false;
            } else if (key == "end_sample") {
                if (!parse_number(track.end_sample)) return false;
            } else if (key == "source_start_sample") {
                if (!parse_number(track.source_start_sample)) return false;
            } else if (key == "source_end_sample") {
                if (!parse_number(track.source_end_sample)) return false;
            } else if (key == "cue_start_frame_75") {
                if (!parse_number(track.cue_start_frame_75)) return false;
            } else if (key == "cue_end_frame_75") {
                if (!parse_number(track.cue_end_frame_75)) return false;
            } else if (key == "cue_has_end_frame_75") {
                if (!parse_bool(track.cue_has_end_frame_75)) return false;
            } else if (key == "source_label") {
                if (!parse_string(track.source_label)) return false;
            } else if (key == "decoded_sample_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint32(value)) return false;
                track.decoded_sample_rate = static_cast<std::uint32_t>(value);
            } else if (key == "decoded_channels") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint16(value)) return false;
                track.decoded_channels = static_cast<std::uint16_t>(value);
            } else if (key == "decoded_bits_per_sample") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint16(value)) return false;
                track.decoded_bits_per_sample = static_cast<std::uint16_t>(value);
            } else if (key == "source_sample_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint32(value)) return false;
                track.source_sample_rate = static_cast<std::uint32_t>(value);
            } else if (key == "source_bits_per_sample") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint16(value)) return false;
                track.source_bits_per_sample = static_cast<std::uint16_t>(value);
            } else if (key == "native_source_available") {
                if (!parse_bool(track.native_source_available)) return false;
            } else if (key == "native_decode") {
                if (!parse_bool(track.native_decode)) return false;
            } else if (key == "lossless_source") {
                if (!parse_bool(track.lossless_source)) return false;
            } else if (key == "lossy_source") {
                if (!parse_bool(track.lossy_source)) return false;
            } else if (key == "resampled") {
                if (!parse_bool(track.resampled)) return false;
            } else if (key == "resampled_from_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint32(value)) return false;
                track.resampled_from_rate = static_cast<std::uint32_t>(value);
            } else if (key == "bitdepth_converted") {
                if (!parse_bool(track.bitdepth_converted)) return false;
            } else if (key == "processed_by_ffmpeg") {
                if (!parse_bool(track.processed_by_ffmpeg)) return false;
            } else if (key == "codec_name") {
                if (!parse_string(track.codec_name)) return false;
            } else if (key == "dsd_source") {
                if (!parse_bool(track.dsd_source)) return false;
            } else if (key == "dsd_sample_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value) || !fits_uint32(value)) return false;
                track.dsd_sample_rate = static_cast<std::uint32_t>(value);
            } else if (key == "cue_track") {
                if (!parse_bool(track.cue_track)) return false;
            } else if (key == "cue_album_end_sample") {
                if (!parse_number(track.cue_album_end_sample)) return false;
            } else if (key == "source_cue_album_end_sample") {
                if (!parse_number(track.source_cue_album_end_sample)) return false;
            } else if (key == "file_dev") {
                if (!parse_number(track.file_dev)) return false;
            } else if (key == "file_ino") {
                if (!parse_number(track.file_ino)) return false;
            } else if (key == "file_size") {
                if (!parse_number(track.file_size)) return false;
            } else if (key == "file_mtime") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.file_mtime = static_cast<std::int64_t>(value);
            } else if (key == "top_level_dev") {
                if (!parse_number(track.top_level_dev)) return false;
            } else if (key == "top_level_ino") {
                if (!parse_number(track.top_level_ino)) return false;
            } else if (key == "top_level_size") {
                if (!parse_number(track.top_level_size)) return false;
            } else if (key == "top_level_mtime") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.top_level_mtime = static_cast<std::int64_t>(value);
            } else if (key == "metadata_state") {
                std::uint64_t value = 0;
                if (!parse_number(value) || value > 2) return false;
                track.metadata_state = static_cast<int>(value);
            } else if (key == "is_stream") {
                if (!parse_bool(track.is_stream)) return false;
            } else if (!skip_value()) {
                return false;
            }

            (void)format_version;
        }
    }

    bool parse_snapshot(PlaylistSessionSnapshot& snapshot) {
        if (!consume('{')) {
            return false;
        }

        int format_version = 0;
        bool version_seen = false;
        bool first = true;
        while (true) {
            skip_whitespace();
            if (consume('}')) {
                break;
            }
            if (!first && !consume(',')) {
                return false;
            }
            first = false;

            std::string key;
            if (!parse_key(key)) {
                return false;
            }

            if (key == "version") {
                std::uint64_t version = 0;
                if (!parse_number(version) || version != PlaylistSessionSnapshot::kFormatVersion) {
                    return false;
                }
                format_version = static_cast<int>(version);
                version_seen = true;
            } else if (key == "current_track_index") {
                std::uint64_t index = 0;
                if (!parse_number(index) || !fits_size(index)) {
                    return false;
                }
                snapshot.current_track_index = static_cast<std::size_t>(index);
            } else if (key == "tracks") {
                if (!consume('[')) {
                    return false;
                }
                bool track_first = true;
                while (true) {
                    skip_whitespace();
                    if (consume(']')) {
                        break;
                    }
                    if (!track_first && !consume(',')) {
                        return false;
                    }
                    track_first = false;
                    if (snapshot.tracks.size() >= PlaylistSessionSnapshot::kMaxTrackCount) {
                        return false;
                    }
                    PlaylistSessionTrack track;
                    if (!parse_track(track, format_version > 0 ? format_version : 2)) {
                        return false;
                    }
                    snapshot.tracks.push_back(std::move(track));
                }
            } else if (!skip_value()) {
                return false;
            }
        }

        if (!version_seen) {
            return false;
        }
        return validate_snapshot(snapshot) && fully_consumed();
    }

private:
    bool skip_value() {
        skip_whitespace();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char ch = text_[pos_];
        if (ch == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (ch == '{') {
            std::size_t depth = 0;
            do {
                if (text_[pos_] == '{') {
                    ++depth;
                } else if (text_[pos_] == '}') {
                    --depth;
                }
                ++pos_;
            } while (pos_ < text_.size() && depth > 0);
            return depth == 0;
        }
        if (ch == '[') {
            std::size_t depth = 0;
            do {
                if (text_[pos_] == '[') {
                    ++depth;
                } else if (text_[pos_] == ']') {
                    --depth;
                }
                ++pos_;
            } while (pos_ < text_.size() && depth > 0);
            return depth == 0;
        }
        while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '}' && text_[pos_] != ']') {
            ++pos_;
        }
        return true;
    }

    std::string text_;
    std::size_t pos_ = 0;
};

} // namespace

std::string PlaylistSession::session_path() {
    const std::string dir = config_directory();
    if (dir.empty()) {
        return {};
    }
    return dir + "/session.json";
}

bool PlaylistSession::load(PlaylistSessionSnapshot& out) const {
    out = PlaylistSessionSnapshot{};
    const std::string path = session_path();
    if (path.empty()) {
        return false;
    }

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::size_t>(size) > PlaylistSessionSnapshot::kMaxFileBytes) {
        return false;
    }
    in.seekg(0, std::ios::beg);

    std::ostringstream buffer;
    buffer << in.rdbuf();
    JsonReader reader(buffer.str());
    if (!reader.parse_snapshot(out)) {
        out = PlaylistSessionSnapshot{};
        return false;
    }
    return !out.tracks.empty();
}

bool PlaylistSession::save(const PlaylistSessionSnapshot& snapshot) const {
    if (!validate_snapshot(snapshot)) {
        return false;
    }

    const std::string dir = config_directory();
    if (dir.empty()) {
        return false;
    }
    if (g_mkdir_with_parents(dir.c_str(), 0700) != 0) {
        return false;
    }

    const std::string path = session_path();
    const std::string tmp_path = path + ".tmp";

    std::ofstream out(tmp_path.c_str(), std::ios::trunc | std::ios::binary);
    if (!out) {
        return false;
    }

    auto write = [&](const std::string& chunk) -> bool {
        out << chunk;
        if (!out.good()) {
            return false;
        }
        const std::streamoff size = out.tellp();
        return size >= 0 && static_cast<std::size_t>(size) <= PlaylistSessionSnapshot::kMaxFileBytes;
    };

    if (!write("{\n") ||
        !write("  \"version\": " + std::to_string(PlaylistSessionSnapshot::kFormatVersion) + ",\n") ||
        !write("  \"current_track_index\": " + std::to_string(snapshot.current_track_index) + ",\n") ||
        !write("  \"tracks\": [\n")) {
        out.close();
        std::remove(tmp_path.c_str());
        return false;
    }

    for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
        std::string line = "    " + serialize_track(snapshot.tracks[i]);
        if (i + 1 < snapshot.tracks.size()) {
            line += ',';
        }
        line += '\n';
        if (!write(line)) {
            out.close();
            std::remove(tmp_path.c_str());
            return false;
        }
    }

    if (!write("  ]\n") || !write("}\n")) {
        out.close();
        std::remove(tmp_path.c_str());
        return false;
    }

    out.flush();
    if (!out.good()) {
        out.close();
        std::remove(tmp_path.c_str());
        return false;
    }
    out.close();
    if (!out.good()) {
        std::remove(tmp_path.c_str());
        return false;
    }

    if (chmod(tmp_path.c_str(), 0600) != 0) {
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

} // namespace pcmtp
