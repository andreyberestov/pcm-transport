#include "pcmtp/patches/PlaylistSessionController.hpp"

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

void PlaylistSessionController::save(const std::vector<PlaylistSessionTrack>& tracks,
                                     std::size_t current_index) const {
    if (tracks.empty()) {
        return;
    }
    PlaylistSessionSnapshot snapshot;
    snapshot.tracks = tracks;
    snapshot.current_track_index = std::min(current_index, tracks.size() - 1);
    PlaylistSession().save(snapshot);
}

bool PlaylistSessionController::load_restore_result(std::vector<PlaylistSessionTrack>& tracks,
                                                    std::size_t& current_index) {
    PlaylistSessionSnapshot snapshot;
    if (!PlaylistSession().load(snapshot)) {
        return false;
    }

    tracks.clear();
    tracks.reserve(snapshot.tracks.size());
    for (const PlaylistSessionTrack& track : snapshot.tracks) {
        if (track.is_stream) {
            continue;
        }
        if (track.audio_file_path.empty()) {
            continue;
        }
        tracks.push_back(track);
    }
    if (tracks.empty()) {
        return false;
    }

    current_index = std::min(snapshot.current_track_index, tracks.size() - 1);
    const PlaylistSessionTrack& saved_target =
        snapshot.tracks[std::min(snapshot.current_track_index, snapshot.tracks.size() - 1)];
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const PlaylistSessionTrack& track = tracks[i];
        if (track.audio_file_path == saved_target.audio_file_path &&
            track.top_level_source_path == saved_target.top_level_source_path &&
            track.start_sample == saved_target.start_sample &&
            track.source_start_sample == saved_target.source_start_sample &&
            track.source_end_sample == saved_target.source_end_sample &&
            track.cue_track == saved_target.cue_track &&
            track.track_number == saved_target.track_number) {
            current_index = i;
            return true;
        }
    }
    return true;
}

bool PlaylistSessionController::restore() {
    std::vector<PlaylistSessionTrack> tracks;
    std::size_t current_index = 0;
    if (!load_restore_result(tracks, current_index)) {
        return false;
    }

    delegate_.apply_restored_session(tracks, current_index);

    auto* focus_data = new FocusRestoreData{&delegate_, current_index};
    g_idle_add(playlist_session_focus_idle_cb, focus_data);
    return true;
}

} // namespace pcmtp
