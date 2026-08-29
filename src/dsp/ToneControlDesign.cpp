// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/dsp/ToneControlDesign.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace pcmtp {
namespace tone {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kMinAuditedHz = 10.0;
constexpr int kResponseAuditPoints = 2048;

ShelfCoefficients make_identity() {
    return ShelfCoefficients{};
}

double clamp_frequency(double hz, int min_hz, int max_hz) {
    return std::max(static_cast<double>(min_hz), std::min(static_cast<double>(max_hz), hz));
}

ShelfCoefficients normalize_coefficients(double b0,
                                         double b1,
                                         double b2,
                                         double a0,
                                         double a1,
                                         double a2) {
    ShelfCoefficients c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

std::complex<double> frequency_response(const ShelfCoefficients& c, double w) {
    const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -w));
    const std::complex<double> z2 = std::exp(std::complex<double>(0.0, -2.0 * w));
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return num / den;
}

ShelfCoefficients make_shelf(bool high,
                             std::uint32_t sample_rate,
                             double gain_db,
                             double cutoff_hz,
                             int min_hz,
                             int max_hz,
                             double slope) {
    if (sample_rate == 0 || std::fabs(gain_db) < 0.001) {
        return make_identity();
    }

    const double hz = clamp_frequency(cutoff_hz, min_hz, max_hz);
    const double nyquist_hz = static_cast<double>(sample_rate) * 0.5;
    if (hz <= 0.0 || hz >= nyquist_hz) {
        return make_identity();
    }

    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = kTwoPi * hz / static_cast<double>(sample_rate);
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = (sin_w0 * 0.5) * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
    const double beta = 2.0 * std::sqrt(A) * alpha;

    if (!high) {
        const double b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + beta);
        const double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
        const double b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - beta);
        const double a0 = (A + 1.0) + (A - 1.0) * cos_w0 + beta;
        const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
        const double a2 = (A + 1.0) + (A - 1.0) * cos_w0 - beta;
        return normalize_coefficients(b0, b1, b2, a0, a1, a2);
    }

    const double b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + beta);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
    const double b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - beta);
    const double a0 = (A + 1.0) - (A - 1.0) * cos_w0 + beta;
    const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
    const double a2 = (A + 1.0) - (A - 1.0) * cos_w0 - beta;
    return normalize_coefficients(b0, b1, b2, a0, a1, a2);
}

} // namespace

int clamp_bass_hz(int hz) {
    return std::max(kBassMinHz, std::min(kBassMaxHz, hz));
}

int clamp_treble_hz(int hz) {
    return std::max(kTrebleMinHz, std::min(kTrebleMaxHz, hz));
}

ShelfCoefficients make_low_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz) {
    return make_shelf(false, sample_rate, gain_db, cutoff_hz, kBassMinHz, kBassMaxHz, kBaxandallBassShelfSlope);
}

ShelfCoefficients make_high_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz) {
    return make_shelf(true, sample_rate, gain_db, cutoff_hz, kTrebleMinHz, kTrebleMaxHz, kBaxandallTrebleShelfSlope);
}

double cascaded_shelf_response_db(std::uint32_t sample_rate,
                                  int bass_db,
                                  int bass_hz,
                                  int treble_db,
                                  int treble_hz,
                                  double hz) {
    if (sample_rate == 0 || hz <= 0.0) {
        return 0.0;
    }
    const ShelfCoefficients low = make_low_shelf(sample_rate, static_cast<double>(bass_db), static_cast<double>(bass_hz));
    const ShelfCoefficients high = make_high_shelf(sample_rate, static_cast<double>(treble_db), static_cast<double>(treble_hz));
    const double w = kTwoPi * hz / static_cast<double>(sample_rate);
    const double mag = std::abs(frequency_response(low, w) * frequency_response(high, w));
    return 20.0 * std::log10(std::max(mag, 1.0e-12));
}

double estimate_cascaded_shelf_max_gain_db(std::uint32_t sample_rate,
                                           int bass_db,
                                           int bass_hz,
                                           int treble_db,
                                           int treble_hz) {
    if (sample_rate == 0 || (bass_db == 0 && treble_db == 0)) {
        return 0.0;
    }

    const double nyquist_hz = static_cast<double>(sample_rate) * 0.5;
    if (nyquist_hz <= kMinAuditedHz) {
        return 0.0;
    }

    double max_db = 0.0;
    const double log_min = std::log(kMinAuditedHz);
    const double log_max = std::log(nyquist_hz);
    for (int i = 0; i < kResponseAuditPoints; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kResponseAuditPoints - 1);
        const double hz = std::exp(log_min + (log_max - log_min) * t);
        const double gain_db = cascaded_shelf_response_db(sample_rate, bass_db, bass_hz, treble_db, treble_hz, hz);
        if (gain_db > max_db) {
            max_db = gain_db;
        }
    }

    return max_db;
}


} // namespace tone
} // namespace pcmtp
