#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pcmtp/session/PlaylistSession.hpp"

namespace pcmtp {

struct SessionFileIdentity {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

enum class SessionPathValidationStatus {
    Ok,
    Missing,
    Changed,
};

struct SessionValidationItem {
    std::size_t index = 0;
    std::string audio_file_path;
    std::string top_level_source_path;
    std::string stable_id;
    SessionFileIdentity saved_audio_identity;
    SessionFileIdentity saved_top_level_identity;
    bool cue_track = false;
};

struct SessionValidationResultItem {
    std::size_t index = 0;
    std::string stable_id;
    SessionPathValidationStatus status = SessionPathValidationStatus::Missing;
};

std::string session_stable_id(const PlaylistSessionTrack& track);
bool capture_session_file_identity(const std::string& path, SessionFileIdentity& identity);
bool is_session_regular_file(const std::string& path, SessionFileIdentity* identity = nullptr);
bool session_file_identity_matches(const SessionFileIdentity& saved, const SessionFileIdentity& current);
SessionPathValidationStatus validate_session_item(const SessionValidationItem& item);
std::vector<SessionValidationItem> build_session_validation_snapshot(
    const std::vector<PlaylistSessionTrack>& tracks);

} // namespace pcmtp
