#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    ChangedAudio,
    ChangedCue,
};

struct SessionValidationItem {
    std::size_t index = 0;
    std::string audio_file_path;
    std::string top_level_source_path;
    std::string stable_id;
    SessionFileIdentity saved_audio_identity;
    SessionFileIdentity saved_top_level_identity;
    bool saved_audio_identity_known = false;
    bool saved_top_level_identity_known = false;
    bool cue_track = false;
};

struct SessionValidationResultItem {
    std::size_t index = 0;
    std::string stable_id;
    SessionPathValidationStatus status = SessionPathValidationStatus::Missing;
    SessionFileIdentity current_audio_identity;
    SessionFileIdentity current_top_level_identity;
    bool current_audio_identity_known = false;
    bool current_top_level_identity_known = false;
};

bool capture_session_file_identity(const std::string& path, SessionFileIdentity& identity);
bool is_session_regular_file(const std::string& path, SessionFileIdentity* identity = nullptr);
bool session_file_identity_matches(const SessionFileIdentity& saved, const SessionFileIdentity& current);
bool session_file_identity_known(const SessionFileIdentity& identity);
SessionValidationResultItem validate_session_item(const SessionValidationItem& item);

} // namespace pcmtp
