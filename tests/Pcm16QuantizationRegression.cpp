// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/core/Pcm16Quantizer.hpp"

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using pcmtp::Pcm16QuantizationMode;
using pcmtp::PcmSample;
using pcmtp::quantize_integer_working_sample_to_pcm16;
using pcmtp::quantize_pcm16_code_units;

void require_equal(const char* label, PcmSample actual, PcmSample expected) {
    if (actual == expected) return;
    std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
    std::exit(EXIT_FAILURE);
}

void test_code_unit_table() {
    struct Row {
        double input;
        PcmSample away_from_zero;
        PcmSample nearest_even;
        PcmSample half_up;
        PcmSample truncate;
    };

    const Row rows[] = {
        { 10.49,  10,  10,  10,  10},
        { 10.50,  11,  10,  11,  10},
        { 10.51,  11,  11,  11,  10},
        { 11.50,  12,  12,  12,  11},
        {-10.49, -10, -10, -10, -11},
        {-10.50, -11, -10, -10, -11},
        {-10.51, -11, -11, -11, -11},
        {-11.50, -12, -12, -11, -12},
    };

    for (const Row& row : rows) {
        require_equal(
            "reference away-from-zero",
            static_cast<PcmSample>(std::llround(row.input)),
            row.away_from_zero);
        require_equal(
            "nearest-even code units",
            quantize_pcm16_code_units(
                row.input, Pcm16QuantizationMode::RoundToNearestEven),
            row.nearest_even);
        require_equal(
            "half-up code units",
            quantize_pcm16_code_units(
                row.input, Pcm16QuantizationMode::RoundHalfUp),
            row.half_up);
        require_equal(
            "truncate code units",
            quantize_pcm16_code_units(
                row.input, Pcm16QuantizationMode::Truncate),
            row.truncate);
    }
}

void test_integer_ties(std::uint16_t working_bits) {
    const std::int64_t divisor = std::int64_t{1} << (working_bits - 16);
    const std::int64_t half = divisor / 2;

    const auto q = [working_bits](std::int64_t value, Pcm16QuantizationMode mode) {
        return quantize_integer_working_sample_to_pcm16(
            static_cast<PcmSample>(value), working_bits, mode);
    };

    require_equal("positive even tie below", q(10 * divisor + half - 1,
                  Pcm16QuantizationMode::RoundToNearestEven), 10);
    require_equal("positive even tie", q(10 * divisor + half,
                  Pcm16QuantizationMode::RoundToNearestEven), 10);
    require_equal("positive even tie above", q(10 * divisor + half + 1,
                  Pcm16QuantizationMode::RoundToNearestEven), 11);
    require_equal("positive odd tie", q(11 * divisor + half,
                  Pcm16QuantizationMode::RoundToNearestEven), 12);

    require_equal("negative even tie below", q(-10 * divisor - half - 1,
                  Pcm16QuantizationMode::RoundToNearestEven), -11);
    require_equal("negative even tie", q(-10 * divisor - half,
                  Pcm16QuantizationMode::RoundToNearestEven), -10);
    require_equal("negative even tie above", q(-10 * divisor - half + 1,
                  Pcm16QuantizationMode::RoundToNearestEven), -10);
    require_equal("negative odd tie", q(-11 * divisor - half,
                  Pcm16QuantizationMode::RoundToNearestEven), -12);

    require_equal("positive half-up tie below", q(10 * divisor + half - 1,
                  Pcm16QuantizationMode::RoundHalfUp), 10);
    require_equal("positive half-up tie", q(10 * divisor + half,
                  Pcm16QuantizationMode::RoundHalfUp), 11);
    require_equal("positive half-up tie above", q(10 * divisor + half + 1,
                  Pcm16QuantizationMode::RoundHalfUp), 11);
    require_equal("negative half-up tie below", q(-10 * divisor - half - 1,
                  Pcm16QuantizationMode::RoundHalfUp), -11);
    require_equal("negative half-up tie", q(-10 * divisor - half,
                  Pcm16QuantizationMode::RoundHalfUp), -10);
    require_equal("negative half-up tie above", q(-10 * divisor - half + 1,
                  Pcm16QuantizationMode::RoundHalfUp), -10);
    require_equal("negative odd half-up tie", q(-11 * divisor - half,
                  Pcm16QuantizationMode::RoundHalfUp), -11);

    require_equal("positive truncate tie", q(10 * divisor + half,
                  Pcm16QuantizationMode::Truncate), 10);
    require_equal("negative truncate tie", q(-10 * divisor - half,
                  Pcm16QuantizationMode::Truncate), -11);
}

void test_exact_widened_s16_values() {
    const PcmSample samples[] = {-32768, -12345, -1, 0, 1, 12345, 32767};
    for (const std::uint16_t working_bits : {std::uint16_t{24}, std::uint16_t{32}}) {
        const std::int64_t scale = std::int64_t{1} << (working_bits - 16);
        for (const PcmSample sample : samples) {
            const PcmSample widened = static_cast<PcmSample>(
                static_cast<std::int64_t>(sample) * scale);
            require_equal(
                "exact widened nearest-even",
                quantize_integer_working_sample_to_pcm16(
                    widened, working_bits, Pcm16QuantizationMode::RoundToNearestEven),
                sample);
            require_equal(
                "exact widened half-up",
                quantize_integer_working_sample_to_pcm16(
                    widened, working_bits, Pcm16QuantizationMode::RoundHalfUp),
                sample);
            require_equal(
                "exact widened truncate",
                quantize_integer_working_sample_to_pcm16(
                    widened, working_bits, Pcm16QuantizationMode::Truncate),
                sample);
        }
    }
}

void test_rounding_mode_independence() {
    const int original_mode = std::fegetround();
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    for (const int mode : modes) {
        if (std::fesetround(mode) != 0) continue;
        require_equal(
            "rounding-mode positive tie",
            quantize_pcm16_code_units(12.5, Pcm16QuantizationMode::RoundToNearestEven),
            12);
        require_equal(
            "rounding-mode positive odd tie",
            quantize_pcm16_code_units(13.5, Pcm16QuantizationMode::RoundToNearestEven),
            14);
        require_equal(
            "rounding-mode negative tie",
            quantize_pcm16_code_units(-12.5, Pcm16QuantizationMode::RoundToNearestEven),
            -12);
        require_equal(
            "rounding-mode negative odd tie",
            quantize_pcm16_code_units(-13.5, Pcm16QuantizationMode::RoundToNearestEven),
            -14);
        require_equal(
            "half-up rounding-mode positive tie",
            quantize_pcm16_code_units(12.5, Pcm16QuantizationMode::RoundHalfUp),
            13);
        require_equal(
            "half-up rounding-mode negative tie",
            quantize_pcm16_code_units(-12.5, Pcm16QuantizationMode::RoundHalfUp),
            -12);
    }
    if (original_mode != -1) {
        std::fesetround(original_mode);
    }
}

void test_boundaries() {
    require_equal("S16 positive saturation",
                  quantize_pcm16_code_units(
                      32767.75, Pcm16QuantizationMode::RoundToNearestEven),
                  32767);
    require_equal("S16 negative saturation",
                  quantize_pcm16_code_units(
                      -32768.75, Pcm16QuantizationMode::RoundToNearestEven),
                  -32768);
    require_equal("S16 positive exact",
                  quantize_pcm16_code_units(
                      32767.0, Pcm16QuantizationMode::RoundToNearestEven),
                  32767);
    require_equal("S16 negative exact",
                  quantize_pcm16_code_units(
                      -32768.0, Pcm16QuantizationMode::RoundToNearestEven),
                  -32768);

    require_equal("S24 maximum saturation",
                  quantize_integer_working_sample_to_pcm16(
                      8388607, 24, Pcm16QuantizationMode::RoundToNearestEven),
                  32767);
    require_equal("S24 minimum",
                  quantize_integer_working_sample_to_pcm16(
                      -8388608, 24, Pcm16QuantizationMode::RoundToNearestEven),
                  -32768);
    require_equal("S32 maximum saturation",
                  quantize_integer_working_sample_to_pcm16(
                      2147483647, 32, Pcm16QuantizationMode::RoundToNearestEven),
                  32767);
    require_equal("S32 minimum",
                  quantize_integer_working_sample_to_pcm16(
                      static_cast<PcmSample>(INT32_MIN), 32,
                      Pcm16QuantizationMode::RoundToNearestEven),
                  -32768);
    require_equal("half-up S16 positive saturation",
                  quantize_pcm16_code_units(
                      32767.75, Pcm16QuantizationMode::RoundHalfUp),
                  32767);
    require_equal("half-up S16 negative saturation",
                  quantize_pcm16_code_units(
                      -32768.75, Pcm16QuantizationMode::RoundHalfUp),
                  -32768);
    require_equal("half-up S24 maximum saturation",
                  quantize_integer_working_sample_to_pcm16(
                      8388607, 24, Pcm16QuantizationMode::RoundHalfUp),
                  32767);
    require_equal("half-up S32 minimum",
                  quantize_integer_working_sample_to_pcm16(
                      static_cast<PcmSample>(INT32_MIN), 32,
                      Pcm16QuantizationMode::RoundHalfUp),
                  -32768);
}

} // namespace

int main() {
    test_code_unit_table();
    test_integer_ties(24);
    test_integer_ties(32);
    test_exact_widened_s16_values();
    test_rounding_mode_independence();
    test_boundaries();
    std::cout << "PCM16 quantization regression: PASS\n";
    return EXIT_SUCCESS;
}
