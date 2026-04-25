#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual void open(const std::string& device_name, const AudioFormat& format) = 0;
    virtual std::size_t write_samples(const PcmSample* samples, std::size_t sample_count) = 0;
    virtual void set_simd_conversion_enabled(bool enabled) { (void)enabled; }
    virtual std::uint64_t simd_conversion_samples_processed() const { return 0; }
    virtual void drain() = 0;
    virtual void close() = 0;
};

} // namespace pcmtp
