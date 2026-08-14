// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>

#include "pcmtp/decoder/SampleBoundary.hpp"

namespace pcmtp {

struct ContainerBoundaryFacts {
    std::string demuxer_name;
    std::string codec_name;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint32_t block_align = 0;
    std::uint16_t bits_per_coded_sample = 0;
    std::int32_t initial_padding = 0;
    std::int32_t trailing_padding = 0;
};

// Performs bounded/structural file-format validation only for the conservative
// exact subsets explicitly supported by the evidence layer. Failure to prove a
// boundary returns Unknown and leaves the caller on its existing fallback path.
SampleExtent verify_container_sample_extent(
    const std::string& path,
    const ContainerBoundaryFacts& facts);

} // namespace pcmtp
