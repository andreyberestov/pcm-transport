#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pcmtp {

struct PlaylistSessionTrack {
    std::string audio_file_path;
    std::string top_level_source_path;
    int track_number = 0;
    std::string title;
    std::string performer;
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::uint64_t source_start_sample = 0;
    std::uint64_t source_end_sample = 0;
    std::uint64_t cue_start_frame_75 = 0;
    std::uint64_t cue_end_frame_75 = 0;
    bool cue_has_end_frame_75 = false;
    std::string source_label;
    std::uint32_t decoded_sample_rate = 44100;
    std::uint16_t decoded_channels = 2;
    std::uint16_t decoded_bits_per_sample = 16;
    std::uint32_t source_sample_rate = 0;
    std::uint16_t source_bits_per_sample = 0;
    bool native_source_available = false;
    bool native_decode = false;
    bool lossless_source = false;
    bool lossy_source = false;
    bool resampled = false;
    std::uint32_t resampled_from_rate = 0;
    bool bitdepth_converted = false;
    bool processed_by_ffmpeg = false;
    std::string codec_name;
    bool dsd_source = false;
    std::uint32_t dsd_sample_rate = 0;
    bool cue_track = false;
    std::uint64_t cue_album_end_sample = 0;
    std::uint64_t source_cue_album_end_sample = 0;
    bool is_stream = false;
    std::uint64_t file_dev = 0;
    std::uint64_t file_ino = 0;
    std::uint64_t file_size = 0;
    std::int64_t file_mtime = 0;
    std::uint64_t top_level_dev = 0;
    std::uint64_t top_level_ino = 0;
    std::uint64_t top_level_size = 0;
    std::int64_t top_level_mtime = 0;
    // Matches GtkPlayerWindow::MetadataState: 0=Pending, 1=Ready, 2=Failed.
    int metadata_state = 1;
};

struct PlaylistSessionSnapshot {
    static constexpr int kFormatVersion = 3;
    static constexpr std::size_t kMaxFileBytes = 32 * 1024 * 1024;
    static constexpr std::size_t kMaxTrackCount = 100000;

    std::size_t current_track_index = 0;
    std::vector<std::string> loaded_source_paths;
    std::vector<PlaylistSessionTrack> tracks;
};

class PlaylistSession {
public:
    static std::string session_path();

    bool load(PlaylistSessionSnapshot& out) const;
    bool save(const PlaylistSessionSnapshot& snapshot) const;
    bool remove() const;
};

} // namespace pcmtp
