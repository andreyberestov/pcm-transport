// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/app/Application.hpp"

#include <iostream>

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/hardware/CardProfileRegistry.hpp"

namespace pcmtp {

int Application::run(int argc, char** argv) {
    std::vector<std::string> gui_source_paths;
    bool probe_only = false;
    bool no_gui = false;
    bool positional_arguments = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!positional_arguments && arg == "--") {
            positional_arguments = true;
        } else if (!positional_arguments && arg == "--probe") {
            probe_only = true;
        } else if (!positional_arguments && arg == "--nogui") {
            no_gui = true;
        } else if (!positional_arguments && (arg == "--help" || arg == "-h")) {
            print_usage(argv[0]);
            return 0;
        } else if (!positional_arguments && !arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        } else {
            gui_source_paths.push_back(arg);
        }
    }

    if (probe_only && !gui_source_paths.empty()) {
        std::cerr << "File arguments cannot be combined with --probe\n";
        print_usage(argv[0]);
        return 1;
    }

    if (probe_only) {
        return run_probe_only();
    }

    if (no_gui) {
        print_usage(argv[0]);
        return 1;
    }

    return run_gui(argv[0] != nullptr ? argv[0] : "pcm_transport",
                   gui_source_paths);
}

int Application::run_probe_only() {
    const auto cards = CardProfileRegistry::probe_cards();
    if (cards.empty()) {
        std::cout << "No ALSA cards found.\n";
        return 0;
    }

    for (const auto& card : cards) {
        std::cout << "Card " << card.card_index << "\n";
        std::cout << "  short:   " << card.short_name << "\n";
        std::cout << "  long:    " << card.long_name << "\n";
        std::cout << "  hw:      " << card.hw_device << "\n";
        std::cout << "  plughw:  " << card.plughw_device << "\n";
        std::cout << "  alsa_hw_profile: " << card.alsa_hw_profile << "\n";
        std::cout << "  low-level features available: "
                  << (card.low_level_features_available ? "yes" : "no") << "\n";
        std::cout << '\n';
    }

    return 0;
}

int Application::run_gui(const std::string& program_name,
                         const std::vector<std::string>& source_paths) {
    GtkPlayerWindow window;
    window.show(program_name, source_paths);
    return 0;
}

void Application::print_usage(const char* program_name) const {
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " [FILE...]\n";
    std::cout << "  " << program_name << " --probe\n";
}

} // namespace pcmtp
