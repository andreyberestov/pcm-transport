#include "pcmtp/session/SessionPathValidation.hpp"

#include <sys/stat.h>

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

bool capture_session_file_identity(const std::string& path, SessionFileIdentity& identity) {
    return stat_regular_file(path, &identity);
}

bool is_session_regular_file(const std::string& path, SessionFileIdentity* identity) {
    return stat_regular_file(path, identity);
}

bool session_file_identity_matches(const SessionFileIdentity& saved, const SessionFileIdentity& current) {
    return saved.dev == current.dev && saved.ino == current.ino && saved.size == current.size &&
           saved.mtime == current.mtime;
}

bool session_file_identity_known(const SessionFileIdentity& identity) {
    return identity.dev != 0 || identity.ino != 0;
}

SessionValidationResultItem validate_session_item(const SessionValidationItem& item) {
    SessionValidationResultItem result;
    result.index = item.index;
    result.stable_id = item.stable_id;

    if (!is_session_regular_file(item.audio_file_path, &result.current_audio_identity)) {
        result.status = SessionPathValidationStatus::Missing;
        return result;
    }
    result.current_audio_identity_known = true;

    bool audio_changed = false;
    if (item.saved_audio_identity_known) {
        audio_changed = !session_file_identity_matches(item.saved_audio_identity, result.current_audio_identity);
    } else {
        audio_changed = true;
    }

    if (item.cue_track && !item.top_level_source_path.empty()) {
        if (!is_session_regular_file(item.top_level_source_path, &result.current_top_level_identity)) {
            result.status = SessionPathValidationStatus::Missing;
            return result;
        }
        result.current_top_level_identity_known = true;

        bool cue_changed = false;
        if (item.saved_top_level_identity_known) {
            cue_changed = !session_file_identity_matches(item.saved_top_level_identity,
                                                         result.current_top_level_identity);
        } else {
            cue_changed = true;
        }

        if (cue_changed) {
            result.status = SessionPathValidationStatus::ChangedCue;
            return result;
        }
    }

    if (audio_changed) {
        result.status = SessionPathValidationStatus::ChangedAudio;
        return result;
    }

    result.status = SessionPathValidationStatus::Ok;
    return result;
}

} // namespace pcmtp
