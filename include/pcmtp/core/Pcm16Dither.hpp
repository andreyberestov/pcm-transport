// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

// Mathematical background for dither, including classic high-pass TPDF:
// U. Zölzer, Digital Audio Signal Processing, 3rd ed., Wiley, 2022, Section 2.2.
// PCM Transport implementations in this module are independently written.
class Pcm16Dither {
public:
    Pcm16Dither(Pcm16DitherMode mode,
                std::uint16_t channels,
                std::uint64_t seed)
        : mode_(mode) {
        configure_filter();
        if (mode_ == Pcm16DitherMode::Off) return;

        const std::size_t channel_count =
            static_cast<std::size_t>(channels == 0 ? 1 : channels);
        states_.resize(channel_count);
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            ChannelState& state = states_[channel];
            const std::uint32_t seed_low = static_cast<std::uint32_t>(seed);
            const std::uint32_t seed_high = static_cast<std::uint32_t>(seed >> 32U);
            const std::uint32_t channel_id = static_cast<std::uint32_t>(channel);
            std::seed_seq channel_seed{
                seed_low,
                seed_high,
                channel_id,
                static_cast<std::uint32_t>(channel_id * 747796405U + 2891336453U)};
            state.generator.seed(channel_seed);
            if (mode_ == Pcm16DitherMode::HighPassClassic) {
                state.previous_uniform = next_uniform53(state);
            } else {
                state.previous_1 = next_tpdf_split32(state);
            }
        }
    }

    bool active() const noexcept {
        return mode_ != Pcm16DitherMode::Off;
    }

    double next_code_units(std::size_t channel) noexcept {
        if (!active() || states_.empty()) return 0.0;
        ChannelState& state = states_[channel];
        if (mode_ == Pcm16DitherMode::HighPassClassic) {
            const double current = next_uniform53(state);
            const double output = current - state.previous_uniform;
            state.previous_uniform = current;
            return output;
        }

        const double current = next_tpdf_split32(state);
        const double output =
            taps_[0] * current + taps_[1] * state.previous_1;
        state.previous_1 = current;
        return output;
    }

private:
    struct ChannelState {
        std::mt19937_64 generator{};
        double previous_uniform = 0.0;
        double previous_1 = 0.0;
    };

    static double next_uniform53(ChannelState& state) noexcept {
        constexpr double kScale = 1.0 / 9007199254740992.0; // 2^53
        return static_cast<double>(state.generator() >> 11U) * kScale;
    }

    static double next_tpdf_split32(ChannelState& state) noexcept {
        constexpr double kScale = 1.0 / 4294967296.0; // 2^32
        const std::uint64_t bits = state.generator();
        const std::int64_t upper = static_cast<std::int64_t>(
            static_cast<std::uint32_t>(bits >> 32U));
        const std::int64_t lower = static_cast<std::int64_t>(
            static_cast<std::uint32_t>(bits));
        return static_cast<double>(upper - lower) * kScale;
    }

    void configure_filter() noexcept {
        if (mode_ == Pcm16DitherMode::HighPassMild) {
            constexpr double kInvSqrt2 = 0.707106781186547524400844362104849039;
            taps_[0] = kInvSqrt2;
            taps_[1] = -kInvSqrt2;
            return;
        }
        taps_ = {{0.0, 0.0}};
    }

    Pcm16DitherMode mode_ = Pcm16DitherMode::Off;
    std::array<double, 2> taps_{{0.0, 0.0}};
    std::vector<ChannelState> states_;
};

inline Pcm16DitherRuntimeKind pcm16_dither_runtime_kind(
    Pcm16DitherMode mode,
    bool final_s16_quantization) noexcept {
    if (!final_s16_quantization) {
        return Pcm16DitherRuntimeKind::NotUsed;
    }
    switch (mode) {
    case Pcm16DitherMode::HighPassClassic:
        return Pcm16DitherRuntimeKind::HighPassClassic;
    case Pcm16DitherMode::HighPassMild:
        return Pcm16DitherRuntimeKind::HighPassMild;
    case Pcm16DitherMode::Off:
    default:
        return Pcm16DitherRuntimeKind::NotUsed;
    }
}

} // namespace pcmtp
