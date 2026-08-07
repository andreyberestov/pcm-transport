// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <atomic>

namespace pcmtp {

class ProbeCancellation {
public:
    std::uint64_t token() const;
    bool cancelled_since(std::uint64_t token) const;
    void cancel();

private:
    std::atomic<std::uint64_t> generation_{0};
};

} // namespace pcmtp
