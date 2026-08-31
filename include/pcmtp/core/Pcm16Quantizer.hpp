// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

// Mathematical background for quantization:
// U. Zölzer, Digital Audio Signal Processing, 3rd ed., Wiley, 2022, Chapter 2.
// PCM Transport implementations in this module are independently written.

inline PcmSample round_pcm16_to_nearest_even(double sample) {
    constexpr double kMinimum = -32768.0;
    constexpr double kMaximum = 32767.0;

    if (sample >= kMaximum) return static_cast<PcmSample>(32767);
    if (sample <= kMinimum) return static_cast<PcmSample>(-32768);

    const double lower = std::floor(sample);
    const double fraction = sample - lower;
    const std::int64_t lower_code = static_cast<std::int64_t>(lower);

    if (fraction < 0.5) {
        return static_cast<PcmSample>(lower_code);
    }
    if (fraction > 0.5) {
        return static_cast<PcmSample>(lower_code + 1);
    }
    return static_cast<PcmSample>(
        (lower_code % 2 == 0) ? lower_code : lower_code + 1);
}

inline PcmSample round_pcm16_half_up(double sample) {
    constexpr double kMinimum = -32768.0;
    constexpr double kMaximum = 32767.0;

    if (sample >= kMaximum) return static_cast<PcmSample>(32767);
    if (sample <= kMinimum) return static_cast<PcmSample>(-32768);

    return static_cast<PcmSample>(std::floor(sample + 0.5));
}

inline PcmSample quantize_pcm16_code_units(
    double sample,
    Pcm16QuantizationMode mode) {
    constexpr double kMinimum = -32768.0;
    constexpr double kMaximum = 32767.0;

    if (sample > kMaximum) sample = kMaximum;
    if (sample < kMinimum) sample = kMinimum;

    if (mode == Pcm16QuantizationMode::Truncate) {
        return static_cast<PcmSample>(std::floor(sample));
    }
    if (mode == Pcm16QuantizationMode::RoundHalfUp) {
        return round_pcm16_half_up(sample);
    }
    return round_pcm16_to_nearest_even(sample);
}

inline PcmSample quantize_integer_working_sample_to_pcm16(
    PcmSample sample,
    std::uint16_t working_bits,
    Pcm16QuantizationMode mode) {
    if (working_bits < 16 || working_bits > 32) {
        throw std::runtime_error("Unsupported PCM precision conversion");
    }
    if (working_bits == 16) {
        if (sample > 32767) return static_cast<PcmSample>(32767);
        if (sample < -32768) return static_cast<PcmSample>(-32768);
        return sample;
    }

    const unsigned shift = static_cast<unsigned>(working_bits - 16);
    const std::int64_t divisor = std::int64_t{1} << shift;
    const std::int64_t value = static_cast<std::int64_t>(sample);

    std::int64_t result = 0;
    if (mode == Pcm16QuantizationMode::Truncate ||
        mode == Pcm16QuantizationMode::RoundHalfUp) {
        result = value / divisor;
        std::int64_t remainder = value % divisor;
        if (remainder < 0) {
            --result;
            remainder += divisor;
        }
        if (mode == Pcm16QuantizationMode::RoundHalfUp &&
            remainder >= divisor / 2) {
            ++result;
        }
    } else {
        const bool negative = value < 0;
        const std::uint64_t magnitude = static_cast<std::uint64_t>(
            negative ? -value : value);
        const std::uint64_t unsigned_divisor =
            static_cast<std::uint64_t>(divisor);
        std::uint64_t quotient = magnitude / unsigned_divisor;
        const std::uint64_t remainder = magnitude % unsigned_divisor;
        const std::uint64_t half = unsigned_divisor / 2U;

        if (remainder > half ||
            (remainder == half && (quotient % 2U) != 0U)) {
            ++quotient;
        }
        result = negative
            ? -static_cast<std::int64_t>(quotient)
            : static_cast<std::int64_t>(quotient);
    }

    if (result > 32767) return static_cast<PcmSample>(32767);
    if (result < -32768) return static_cast<PcmSample>(-32768);
    return static_cast<PcmSample>(result);
}

inline PcmSample quantize_integer_working_sample_to_pcm16_with_dither(
    PcmSample sample,
    std::uint16_t working_bits,
    Pcm16QuantizationMode mode,
    double dither_code_units) {
    if (working_bits < 16 || working_bits > 32) {
        throw std::runtime_error("Unsupported PCM precision conversion");
    }
    if (working_bits == 16) {
        return quantize_pcm16_code_units(
            static_cast<double>(sample) + dither_code_units, mode);
    }

    const unsigned shift = static_cast<unsigned>(working_bits - 16);
    const double divisor = static_cast<double>(std::uint64_t{1} << shift);
    const double code_units = static_cast<double>(sample) / divisor;
    return quantize_pcm16_code_units(code_units + dither_code_units, mode);
}

} // namespace pcmtp
