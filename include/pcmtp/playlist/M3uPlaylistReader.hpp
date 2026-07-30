// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <vector>

namespace pcmtp {

class M3uPlaylistReader {
public:
    static bool looks_like_playlist_path(const std::string& path);
    static std::vector<std::string> read_local_paths(const std::string& path);
};

} // namespace pcmtp
