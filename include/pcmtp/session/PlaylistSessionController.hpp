#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "pcmtp/session/PlaylistSession.hpp"

namespace pcmtp {

class PlaylistSessionController {
public:
    struct RestoreResult {
        std::vector<PlaylistSessionTrack> tracks;
        std::vector<std::string> loaded_source_paths;
        std::size_t current_index = 0;
    };

    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual bool ui_closing() const = 0;
        virtual void apply_restored_session(const std::vector<PlaylistSessionTrack>& tracks,
                                            std::size_t current_index,
                                            const std::vector<std::string>& loaded_source_paths) = 0;
        virtual void finalize_focus_restore(std::size_t index) = 0;
    };

    explicit PlaylistSessionController(Delegate& delegate);

    bool save(const std::vector<PlaylistSessionTrack>& tracks,
              std::size_t current_index,
              const std::vector<std::string>& loaded_source_paths) const;
    bool restore();

    static bool load_restore_result(RestoreResult& out);

private:
    Delegate& delegate_;
};

} // namespace pcmtp
