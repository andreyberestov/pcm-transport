// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/dsp/ToneControlDesign.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>

namespace pcmtp {
namespace tone {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr std::size_t kMaximumPoles = 4;
constexpr std::size_t kMaximumNumeratorDegree = 4;
constexpr std::size_t kPeakBoundMaximumIterations = 32768;
constexpr double kPeakBoundRelativeTailTolerance = 1.0e-12;
constexpr double kPeakBoundRelativeSafetyMargin = 1.0e-8;

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

bool is_identity(const ShelfCoefficients& c) {
    return c.b0 == 1.0 && c.b1 == 0.0 && c.b2 == 0.0 &&
           c.a1 == 0.0 && c.a2 == 0.0;
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

void append_numerator_section(const ShelfCoefficients& c,
                              std::array<double, kMaximumNumeratorDegree + 1>& coefficients,
                              std::size_t& degree) {
    std::array<double, kMaximumNumeratorDegree + 1> next{};
    for (std::size_t i = 0; i <= degree; ++i) {
        next[i] += coefficients[i] * c.b0;
        next[i + 1] += coefficients[i] * c.b1;
        next[i + 2] += coefficients[i] * c.b2;
    }
    coefficients = next;
    degree += 2;
}

bool append_denominator_poles(const ShelfCoefficients& c,
                              std::array<std::complex<double>, kMaximumPoles>& poles,
                              std::size_t& pole_count) {
    if (pole_count + 2 > poles.size()) {
        return false;
    }

    const std::complex<double> discriminant(c.a1 * c.a1 - 4.0 * c.a2, 0.0);
    const std::complex<double> root = std::sqrt(discriminant);
    poles[pole_count++] = (-c.a1 + root) * 0.5;
    poles[pole_count++] = (-c.a1 - root) * 0.5;
    return true;
}

std::complex<double> evaluate_polynomial(
    const std::array<double, kMaximumNumeratorDegree + 1>& coefficients,
    std::size_t degree,
    const std::complex<double>& z) {
    std::complex<double> value(coefficients[0], 0.0);
    for (std::size_t i = 1; i <= degree; ++i) {
        value = value * z + coefficients[i];
    }
    return value;
}

// Mathematical background for the LTI bounded-input/bounded-output peak bound:
// A. V. Oppenheim and R. W. Schafer, Discrete-Time Signal Processing,
// 3rd ed., Section 2.4.
// PCM Transport implementation in this module is independently written.
double cascaded_impulse_l1_upper_bound(const ShelfCoefficients& low,
                                        const ShelfCoefficients& high) {
    std::array<double, kMaximumNumeratorDegree + 1> numerator{};
    numerator[0] = 1.0;
    std::size_t numerator_degree = 0;

    std::array<std::complex<double>, kMaximumPoles> poles{};
    std::size_t pole_count = 0;

    if (!is_identity(low)) {
        append_numerator_section(low, numerator, numerator_degree);
        if (!append_denominator_poles(low, poles, pole_count)) {
            return std::numeric_limits<double>::infinity();
        }
    }
    if (!is_identity(high)) {
        append_numerator_section(high, numerator, numerator_degree);
        if (!append_denominator_poles(high, poles, pole_count)) {
            return std::numeric_limits<double>::infinity();
        }
    }

    if (pole_count == 0) {
        return 1.0;
    }

    std::array<std::complex<double>, kMaximumPoles> residues{};
    std::array<std::complex<double>, kMaximumPoles> powers{};
    std::array<double, kMaximumPoles> pole_radii{};

    for (std::size_t i = 0; i < pole_count; ++i) {
        const double radius = std::abs(poles[i]);
        if (!std::isfinite(radius) || radius >= 1.0) {
            return std::numeric_limits<double>::infinity();
        }
        pole_radii[i] = radius;

        std::complex<double> derivative(1.0, 0.0);
        for (std::size_t j = 0; j < pole_count; ++j) {
            if (i != j) {
                derivative *= poles[i] - poles[j];
            }
        }
        if (!std::isfinite(std::abs(derivative)) || std::abs(derivative) <= 1.0e-18) {
            return std::numeric_limits<double>::infinity();
        }

        residues[i] = evaluate_polynomial(numerator, numerator_degree, poles[i]) / derivative;
        if (!std::isfinite(std::abs(residues[i]))) {
            return std::numeric_limits<double>::infinity();
        }
        powers[i] = std::complex<double>(1.0, 0.0);
    }

    double prefix_sum = std::fabs(numerator[0]);
    double tail_bound = std::numeric_limits<double>::infinity();

    for (std::size_t n = 1; n <= kPeakBoundMaximumIterations; ++n) {
        std::complex<double> sample(0.0, 0.0);
        for (std::size_t i = 0; i < pole_count; ++i) {
            sample += residues[i] * powers[i];
        }
        prefix_sum += std::abs(sample);

        tail_bound = 0.0;
        for (std::size_t i = 0; i < pole_count; ++i) {
            powers[i] *= poles[i];
            tail_bound += std::abs(residues[i]) * std::abs(powers[i]) /
                          (1.0 - pole_radii[i]);
        }

        if (!std::isfinite(prefix_sum) || !std::isfinite(tail_bound)) {
            return std::numeric_limits<double>::infinity();
        }
        if (tail_bound <= std::max(1.0e-15,
                                   prefix_sum * kPeakBoundRelativeTailTolerance)) {
            break;
        }
    }

    const double bound = (prefix_sum + tail_bound) *
                         (1.0 + kPeakBoundRelativeSafetyMargin);
    return std::max(1.0, bound);
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

double estimate_cascaded_shelf_peak_bound_db(std::uint32_t sample_rate,
                                              int bass_db,
                                              int bass_hz,
                                              int treble_db,
                                              int treble_hz) {
    if (sample_rate == 0 || (bass_db == 0 && treble_db == 0)) {
        return 0.0;
    }

    const ShelfCoefficients low = make_low_shelf(
        sample_rate, static_cast<double>(bass_db), static_cast<double>(bass_hz));
    const ShelfCoefficients high = make_high_shelf(
        sample_rate, static_cast<double>(treble_db), static_cast<double>(treble_hz));
    double l1_bound = cascaded_impulse_l1_upper_bound(low, high);
    if (!std::isfinite(l1_bound)) {
        const ShelfCoefficients identity = make_identity();
        const double low_bound = cascaded_impulse_l1_upper_bound(low, identity);
        const double high_bound = cascaded_impulse_l1_upper_bound(identity, high);
        if (!std::isfinite(low_bound) || !std::isfinite(high_bound) ||
            low_bound > std::numeric_limits<double>::max() / high_bound) {
            return std::numeric_limits<double>::infinity();
        }
        l1_bound = low_bound * high_bound;
    }
    return 20.0 * std::log10(std::max(1.0, l1_bound));
}

} // namespace tone
} // namespace pcmtp
