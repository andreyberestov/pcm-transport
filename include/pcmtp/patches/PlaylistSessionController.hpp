#pragma once

#include <cstddef>
#include <vector>

typedef int gboolean;
typedef void* gpointer;

#include "pcmtp/session/PlaylistSession.hpp"

namespace pcmtp {

class PlaylistSessionController {
    friend gboolean playlist_session_focus_idle_cb(gpointer data);

public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual bool ui_closing() const = 0;
        virtual void apply_restored_session(const std::vector<PlaylistSessionTrack>& tracks,
                                            std::size_t current_index) = 0;
        virtual void finalize_focus_restore(std::size_t index) = 0;
    };

    explicit PlaylistSessionController(Delegate& delegate);

    void save(const std::vector<PlaylistSessionTrack>& tracks, std::size_t current_index) const;
    bool restore();

    static bool load_restore_result(std::vector<PlaylistSessionTrack>& tracks, std::size_t& current_index);

private:
    Delegate& delegate_;
};

} // namespace pcmtp
