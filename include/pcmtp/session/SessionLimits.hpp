#pragma once

#include <cstdint>

namespace pcmtp {

// Matches GtkPlayerWindow::kSelectablePcmRates and AlsaPcmBackend supported output rates.
constexpr std::uint32_t kMaxPcmSampleRate = 1536000U;

// Matches GtkPlayerWindow::kDsdRateDefinitions (DSD1024).
constexpr std::uint32_t kMaxDsdSampleRate = 45158400U;

inline bool is_supported_pcm_sample_rate(std::uint32_t sample_rate) {
    return sample_rate == 44100U || sample_rate == 48000U || sample_rate == 88200U ||
           sample_rate == 96000U || sample_rate == 176400U || sample_rate == 192000U ||
           sample_rate == 352800U || sample_rate == 384000U || sample_rate == 705600U ||
           sample_rate == 768000U || sample_rate == 1411200U || sample_rate == 1536000U;
}

inline bool is_supported_dsd_sample_rate(std::uint32_t sample_rate) {
    return sample_rate == 2822400U || sample_rate == 5644800U || sample_rate == 11289600U ||
           sample_rate == 22579200U || sample_rate == 45158400U || sample_rate == 6144000U ||
           sample_rate == 12288000U;
}

} // namespace pcmtp
