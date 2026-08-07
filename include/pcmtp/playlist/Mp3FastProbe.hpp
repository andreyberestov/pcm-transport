// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"

namespace pcmtp {

enum class Mp3FastProbeStatus {
    Complete,
    NeedExternalFallback,
    Invalid
};

struct Mp3FastProbeResult {
    Mp3FastProbeStatus status = Mp3FastProbeStatus::NeedExternalFallback;
    ExternalAudioInfo info{};
    std::string diagnostic;
};

Mp3FastProbeResult probe_mp3_fast(const std::string& path);

} // namespace pcmtp
