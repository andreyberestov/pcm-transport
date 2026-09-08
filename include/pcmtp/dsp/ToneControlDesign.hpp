// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace pcmtp {
namespace tone {

struct ShelfCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};


constexpr int kBassMinHz = 40;
constexpr int kBassMaxHz = 220;
constexpr int kTrebleMinHz = 3000;
constexpr int kTrebleMaxHz = 16000;
constexpr double kBaxandallBassShelfSlope = 1.0;
constexpr double kBaxandallTrebleShelfSlope = 1.0;

int clamp_bass_hz(int hz);
int clamp_treble_hz(int hz);
ShelfCoefficients make_low_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz);
ShelfCoefficients make_high_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz);
double cascaded_shelf_response_db(std::uint32_t sample_rate,
                                  int bass_db,
                                  int bass_hz,
                                  int treble_db,
                                  int treble_hz,
                                  double hz);
double estimate_cascaded_shelf_peak_bound_db(std::uint32_t sample_rate,
                                              int bass_db,
                                              int bass_hz,
                                              int treble_db,
                                              int treble_hz);

} // namespace tone
} // namespace pcmtp
