// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/util/ProbeCancellation.hpp"

namespace pcmtp {

std::uint64_t ProbeCancellation::token() const {
    return generation_.load(std::memory_order_acquire);
}

bool ProbeCancellation::cancelled_since(std::uint64_t token_value) const {
    return generation_.load(std::memory_order_acquire) != token_value;
}

void ProbeCancellation::cancel() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace pcmtp
