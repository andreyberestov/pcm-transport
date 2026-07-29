#include "pcmtp/session/SessionPathValidation.hpp"

#include <sys/stat.h>

#include <sstream>

namespace pcmtp {

namespace {

bool stat_regular_file(const std::string& path, SessionFileIdentity* identity) {
    if (path.empty()) {
        return false;
    }
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (identity != nullptr) {
        identity->dev = static_cast<std::uint64_t>(st.st_dev);
        identity->ino = static_cast<std::uint64_t>(st.st_ino);
        identity->size = static_cast<std::uint64_t>(st.st_size);
        identity->mtime = static_cast<std::int64_t>(st.st_mtime);
    }
    return true;
}

} // namespace

std::string session_stable_id(const PlaylistSessionTrack& track) {
    std::ostringstream out;
    out << track.audio_file_path << '\x1e'
        << track.top_level_source_path << '\x1e'
        << track.track_number << '\x1e'
        << track.start_sample << '\x1e'
        << track.source_start_sample << '\x1e'
        << track.source_end_sample << '\x1e'
        << track.cue_start_frame_75 << '\x1e'
        << track.cue_end_frame_75 << '\x1e'
        << (track.cue_track ? 1 : 0);
    return out.str();
}

bool capture_session_file_identity(const std::string& path, SessionFileIdentity& identity) {
    return stat_regular_file(path, &identity);
}

bool is_session_regular_file(const std::string& path, SessionFileIdentity* identity) {
    return stat_regular_file(path, identity);
}

bool session_file_identity_matches(const SessionFileIdentity& saved, const SessionFileIdentity& current) {
    if (saved.dev == 0 && saved.ino == 0) {
        return true;
    }
    return saved.dev == current.dev && saved.ino == current.ino && saved.size == current.size &&
           saved.mtime == current.mtime;
}

SessionPathValidationStatus validate_session_item(const SessionValidationItem& item) {
    SessionFileIdentity audio_identity;
    if (!is_session_regular_file(item.audio_file_path, &audio_identity)) {
        return SessionPathValidationStatus::Missing;
    }
    if (!session_file_identity_matches(item.saved_audio_identity, audio_identity)) {
        if (item.saved_audio_identity.dev != 0 || item.saved_audio_identity.ino != 0) {
            return SessionPathValidationStatus::Changed;
        }
    }

    if (item.cue_track && !item.top_level_source_path.empty()) {
        SessionFileIdentity cue_identity;
        if (!is_session_regular_file(item.top_level_source_path, &cue_identity)) {
            return SessionPathValidationStatus::Missing;
        }
        if (!session_file_identity_matches(item.saved_top_level_identity, cue_identity)) {
            if (item.saved_top_level_identity.dev != 0 || item.saved_top_level_identity.ino != 0) {
                return SessionPathValidationStatus::Changed;
            }
        }
    }

    return SessionPathValidationStatus::Ok;
}

std::vector<SessionValidationItem> build_session_validation_snapshot(
    const std::vector<PlaylistSessionTrack>& tracks) {
    std::vector<SessionValidationItem> items;
    items.reserve(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const PlaylistSessionTrack& track = tracks[index];
        SessionValidationItem item;
        item.index = index;
        item.audio_file_path = track.audio_file_path;
        item.top_level_source_path = track.top_level_source_path;
        item.stable_id = session_stable_id(track);
        item.cue_track = track.cue_track;
        item.saved_audio_identity.dev = track.file_dev;
        item.saved_audio_identity.ino = track.file_ino;
        item.saved_audio_identity.size = track.file_size;
        item.saved_audio_identity.mtime = track.file_mtime;
        item.saved_top_level_identity.dev = track.top_level_dev;
        item.saved_top_level_identity.ino = track.top_level_ino;
        item.saved_top_level_identity.size = track.top_level_size;
        item.saved_top_level_identity.mtime = track.top_level_mtime;
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace pcmtp
