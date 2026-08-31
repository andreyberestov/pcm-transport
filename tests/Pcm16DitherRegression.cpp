// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/core/Pcm16Dither.hpp"
#include "pcmtp/core/Pcm16Quantizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using pcmtp::Pcm16Dither;
using pcmtp::Pcm16DitherMode;
using pcmtp::Pcm16DitherRuntimeKind;
using pcmtp::Pcm16QuantizationMode;
using pcmtp::PcmSample;
using pcmtp::pcm16_dither_runtime_kind;
using pcmtp::quantize_integer_working_sample_to_pcm16_with_dither;

void fail(const char* label) {
    std::cerr << label << '\n';
    std::exit(EXIT_FAILURE);
}

void require(bool condition, const char* label) {
    if (!condition) fail(label);
}

void test_off() {
    Pcm16Dither dither(Pcm16DitherMode::Off, 2, UINT64_C(1234));
    for (int i = 0; i < 1000; ++i) {
        require(dither.next_code_units(static_cast<std::size_t>(i % 2)) == 0.0,
                "Off dither produced non-zero output");
    }
}

void test_runtime_contract() {
    require(
        pcm16_dither_runtime_kind(
            Pcm16DitherMode::HighPassClassic,
            true) == Pcm16DitherRuntimeKind::HighPassClassic,
        "Classic dither runtime kind is incorrect");
    require(
        pcm16_dither_runtime_kind(
            Pcm16DitherMode::HighPassMild,
            true) == Pcm16DitherRuntimeKind::HighPassMild,
        "Mild dither runtime kind is incorrect");
    require(
        pcm16_dither_runtime_kind(
            Pcm16DitherMode::HighPassMild,
            false) == Pcm16DitherRuntimeKind::NotUsed,
        "Non-quantizing path activated dither");
    require(
        pcm16_dither_runtime_kind(
            Pcm16DitherMode::Off,
            true) == Pcm16DitherRuntimeKind::NotUsed,
        "Off dither reported active");
}

void test_classic_highpass_shape() {
    constexpr int kSamples = 400000;
    Pcm16Dither dither(Pcm16DitherMode::HighPassClassic, 1,
                       UINT64_C(0x4f6c7a6572545044));
    long double sum_squares = 0.0L;
    long double sum_lag_one = 0.0L;
    double previous = dither.next_code_units(0);
    require(std::fabs(previous) < 1.0,
            "Classic HP-TPDF exceeded its one-LSB peak bound");
    for (int i = 1; i < kSamples; ++i) {
        const double current = dither.next_code_units(0);
        require(std::fabs(current) < 1.0,
                "Classic HP-TPDF exceeded its one-LSB peak bound");
        sum_squares += current * current;
        sum_lag_one += current * previous;
        previous = current;
    }
    const double lag_one_correlation = static_cast<double>(
        sum_lag_one / sum_squares);
    require(lag_one_correlation > -0.52 && lag_one_correlation < -0.48,
            "Classic HP-TPDF first-difference correlation changed unexpectedly");
}

void test_profile_statistics(Pcm16DitherMode mode) {
    constexpr int kSamples = 600000;
    Pcm16Dither dither(mode, 1, UINT64_C(0x123456789abcdef0));
    long double sum = 0.0L;
    long double sum_squares = 0.0L;
    double maximum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const double value = dither.next_code_units(0);
        require(std::isfinite(value), "Dither produced a non-finite sample");
        sum += value;
        sum_squares += value * value;
        maximum = std::max(maximum, std::fabs(value));
    }
    const double mean = static_cast<double>(sum / kSamples);
    const double rms = std::sqrt(static_cast<double>(sum_squares / kSamples));
    require(std::fabs(mean) < 0.01, "Dither mean is not near zero");
    require(rms > 0.39 && rms < 0.43, "Dither RMS changed unexpectedly");
    require(maximum < 1.45, "Retained dither peak exceeded the expected bound");
}

void test_mild_shape() {
    constexpr int kSamples = 800000;
    Pcm16Dither dither(Pcm16DitherMode::HighPassMild, 1,
                       UINT64_C(0x4d696c6433326269));
    long double sum = 0.0L;
    long double sum_squares = 0.0L;
    long double sum_lag_one = 0.0L;
    long double sum_lag_two = 0.0L;
    double previous_2 = dither.next_code_units(0);
    double previous_1 = dither.next_code_units(0);
    sum += previous_2 + previous_1;
    sum_squares += previous_2 * previous_2 + previous_1 * previous_1;
    for (int i = 2; i < kSamples; ++i) {
        const double current = dither.next_code_units(0);
        sum += current;
        sum_squares += current * current;
        sum_lag_one += current * previous_1;
        sum_lag_two += current * previous_2;
        previous_2 = previous_1;
        previous_1 = current;
    }
    const double mean = static_cast<double>(sum / kSamples);
    const double mean_square = static_cast<double>(sum_squares / kSamples);
    const double lag_one = static_cast<double>(
        sum_lag_one / static_cast<long double>(kSamples - 2)) / mean_square;
    const double lag_two = static_cast<double>(
        sum_lag_two / static_cast<long double>(kSamples - 2)) / mean_square;
    require(std::fabs(mean) < 0.01, "Mild dither mean is not near zero");
    require(lag_one > -0.515 && lag_one < -0.485,
            "Mild first-lag correlation changed unexpectedly");
    require(std::fabs(lag_two) < 0.015,
            "Mild second-lag correlation changed unexpectedly");
}

void test_mild_channel_correlation() {
    constexpr int kSamples = 400000;
    Pcm16Dither dither(Pcm16DitherMode::HighPassMild, 2,
                       UINT64_C(0x4368616e6e656c73));
    long double left_sum = 0.0L;
    long double right_sum = 0.0L;
    long double left_squares = 0.0L;
    long double right_squares = 0.0L;
    long double cross_sum = 0.0L;
    for (int i = 0; i < kSamples; ++i) {
        const long double left = dither.next_code_units(0);
        const long double right = dither.next_code_units(1);
        left_sum += left;
        right_sum += right;
        left_squares += left * left;
        right_squares += right * right;
        cross_sum += left * right;
    }
    const long double left_mean = left_sum / kSamples;
    const long double right_mean = right_sum / kSamples;
    const long double left_variance =
        left_squares / kSamples - left_mean * left_mean;
    const long double right_variance =
        right_squares / kSamples - right_mean * right_mean;
    const long double covariance =
        cross_sum / kSamples - left_mean * right_mean;
    const long double correlation = covariance /
        std::sqrt(left_variance * right_variance);
    require(std::fabs(static_cast<double>(correlation)) < 0.02,
            "Mild dither channels are unexpectedly correlated");
}

void test_truncate_with_dither() {
    const PcmSample floor_semantics =
        quantize_integer_working_sample_to_pcm16_with_dither(
            static_cast<PcmSample>(10 * 65536 + 16384), 32,
            Pcm16QuantizationMode::Truncate, 0.5);
    const PcmSample positive_crossing =
        quantize_integer_working_sample_to_pcm16_with_dither(
            static_cast<PcmSample>(10 * 65536 + 16384), 32,
            Pcm16QuantizationMode::Truncate, 0.8);
    const PcmSample negative_crossing =
        quantize_integer_working_sample_to_pcm16_with_dither(
            static_cast<PcmSample>(-10 * 65536 - 16384), 32,
            Pcm16QuantizationMode::Truncate, 0.5);
    require(floor_semantics == 10, "Truncate+dither used rounding semantics");
    require(positive_crossing == 11, "Positive dither was not applied before Truncate");
    require(negative_crossing == -10, "Negative sample dither was not applied before Truncate");
}

void test_half_up_with_dither() {
    const PcmSample positive_tie =
        quantize_integer_working_sample_to_pcm16_with_dither(
            static_cast<PcmSample>(10 * 65536), 32,
            Pcm16QuantizationMode::RoundHalfUp, 0.5);
    const PcmSample negative_tie =
        quantize_integer_working_sample_to_pcm16_with_dither(
            static_cast<PcmSample>(-10 * 65536), 32,
            Pcm16QuantizationMode::RoundHalfUp, -0.5);
    require(positive_tie == 11,
            "Round half up+dither did not round positive tie upward");
    require(negative_tie == -10,
            "Round half up+dither did not round negative tie toward +infinity");
}

void test_integer_boundary_saturation() {
    const PcmSample maximum = quantize_integer_working_sample_to_pcm16_with_dither(
        std::numeric_limits<PcmSample>::max(), 32,
        Pcm16QuantizationMode::RoundToNearestEven, 1.4);
    const PcmSample minimum = quantize_integer_working_sample_to_pcm16_with_dither(
        std::numeric_limits<PcmSample>::min(), 32,
        Pcm16QuantizationMode::RoundToNearestEven, -1.4);
    require(maximum == 32767, "Positive dither boundary did not saturate");
    require(minimum == -32768, "Negative dither boundary did not saturate");
}

} // namespace

int main() {
    test_off();
    test_runtime_contract();
    test_profile_statistics(Pcm16DitherMode::HighPassClassic);
    test_profile_statistics(Pcm16DitherMode::HighPassMild);
    test_classic_highpass_shape();
    test_mild_shape();
    test_mild_channel_correlation();
    test_truncate_with_dither();
    test_half_up_with_dither();
    test_integer_boundary_saturation();
    std::cout << "PCM16 dither regression: PASS\n";
    return EXIT_SUCCESS;
}
