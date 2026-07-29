#include "pcmtp/session/PlaylistSessionController.hpp"

#include <gtk/gtk.h>

#include <algorithm>

namespace pcmtp {
namespace {

struct FocusRestoreData {
    PlaylistSessionController::Delegate* delegate = nullptr;
    std::size_t index = 0;
};

} // namespace

gboolean playlist_session_focus_idle_cb(gpointer data) {
    auto* focus_data = static_cast<FocusRestoreData*>(data);
    if (focus_data != nullptr && focus_data->delegate != nullptr && !focus_data->delegate->ui_closing()) {
        focus_data->delegate->finalize_focus_restore(focus_data->index);
    }
    delete focus_data;
    return G_SOURCE_REMOVE;
}

PlaylistSessionController::PlaylistSessionController(Delegate& delegate) : delegate_(delegate) {}

bool PlaylistSessionController::save(const std::vector<PlaylistSessionTrack>& tracks,
                                     std::size_t current_index,
                                     const std::vector<std::string>& loaded_source_paths) const {
    if (tracks.empty()) {
        return false;
    }
    PlaylistSessionSnapshot snapshot;
    snapshot.tracks = tracks;
    snapshot.current_track_index = std::min(current_index, tracks.size() - 1);
    snapshot.loaded_source_paths = loaded_source_paths;
    return PlaylistSession().save(snapshot);
}

bool PlaylistSessionController::load_restore_result(RestoreResult& out) {
    out = RestoreResult{};
    PlaylistSessionSnapshot snapshot;
    if (!PlaylistSession().load(snapshot)) {
        return false;
    }

    out.loaded_source_paths = snapshot.loaded_source_paths;
    out.tracks.reserve(snapshot.tracks.size());
    for (const PlaylistSessionTrack& track : snapshot.tracks) {
        if (track.is_stream || track.audio_file_path.empty()) {
            continue;
        }
        out.tracks.push_back(track);
    }
    if (out.tracks.empty()) {
        return false;
    }

    out.current_index = std::min(snapshot.current_track_index, out.tracks.size() - 1);
    const PlaylistSessionTrack& saved_target =
        snapshot.tracks[std::min(snapshot.current_track_index, snapshot.tracks.size() - 1)];
    for (std::size_t i = 0; i < out.tracks.size(); ++i) {
        const PlaylistSessionTrack& track = out.tracks[i];
        if (track.audio_file_path == saved_target.audio_file_path &&
            track.top_level_source_path == saved_target.top_level_source_path &&
            track.start_sample == saved_target.start_sample &&
            track.source_start_sample == saved_target.source_start_sample &&
            track.source_end_sample == saved_target.source_end_sample &&
            track.cue_track == saved_target.cue_track &&
            track.track_number == saved_target.track_number) {
            out.current_index = i;
            return true;
        }
    }
    return true;
}

bool PlaylistSessionController::restore() {
    RestoreResult result;
    if (!load_restore_result(result)) {
        return false;
    }

    delegate_.apply_restored_session(result.tracks, result.current_index, result.loaded_source_paths);

    auto* focus_data = new FocusRestoreData{&delegate_, result.current_index};
    g_idle_add(playlist_session_focus_idle_cb, focus_data);
    return true;
}

} // namespace pcmtp
