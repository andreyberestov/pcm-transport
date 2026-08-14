// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <vector>

namespace pcmtp {

class Application {
public:
    int run(int argc, char** argv);

private:
    int run_probe_only();
    int run_gui(const std::string& program_name,
                const std::vector<std::string>& source_paths);
    void print_usage(const char* program_name) const;
};

} // namespace pcmtp
