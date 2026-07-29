#pragma once

#include <array>
#include <cstdint>

namespace pcmtp {

struct DsdRateDefinition {
    std::uint32_t dsd_sample_rate;
    std::uint32_t ffmpeg_pcm_rate;
    std::uint32_t default_pcm_rate;
    const char* source_label;
    const char* ffmpeg_label;
    bool family_441;
};

inline constexpr std::array<DsdRateDefinition, 10> kDsdRateDefinitions = {{
    {2822400U, 352800U, 176400U, "DSD64 · 2.8224 MHz", "352.8 kHz", true},
    {5644800U, 705600U, 176400U, "DSD128 · 5.6448 MHz", "705.6 kHz", true},
    {11289600U, 1411200U, 176400U, "DSD256 · 11.2896 MHz", "1411.2 kHz", true},
    {22579200U, 2822400U, 176400U, "DSD512 · 22.5792 MHz", "2822.4 kHz", true},
    {45158400U, 5644800U, 176400U, "DSD1024 · 45.1584 MHz", "5644.8 kHz", true},
    {3072000U, 384000U, 192000U, "DSD64 · 3.072 MHz", "384 kHz", false},
    {6144000U, 768000U, 192000U, "DSD128 · 6.144 MHz", "768 kHz", false},
    {12288000U, 1536000U, 192000U, "DSD256 · 12.288 MHz", "1536 kHz", false},
    {24576000U, 3072000U, 192000U, "DSD512 · 24.576 MHz", "3072 kHz", false},
    {49152000U, 6144000U, 192000U, "DSD1024 · 49.152 MHz", "6144 kHz", false},
}};

inline constexpr std::uint32_t kMaxDsdSampleRate = 49152000U;

inline const DsdRateDefinition* find_dsd_rate_definition(std::uint32_t dsd_sample_rate) {
    for (const DsdRateDefinition& definition : kDsdRateDefinitions) {
        if (definition.dsd_sample_rate == dsd_sample_rate) {
            return &definition;
        }
    }
    return nullptr;
}

inline bool is_supported_dsd_sample_rate(std::uint32_t sample_rate) {
    return find_dsd_rate_definition(sample_rate) != nullptr;
}

} // namespace pcmtp
