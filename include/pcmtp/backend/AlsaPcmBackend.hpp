#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "pcmtp/backend/IAudioBackend.hpp"

namespace pcmtp {

class AlsaPcmBackend final : public IAudioBackend {
public:
    AlsaPcmBackend() = default;
    ~AlsaPcmBackend() override;

    void open(const std::string& device_name, const AudioFormat& format) override;
    std::size_t write_samples(const PcmSample* samples, std::size_t sample_count) override;
    void set_simd_conversion_enabled(bool enabled) override;
    std::uint64_t simd_conversion_samples_processed() const override;
    void drain() override;
    void close() override;

    snd_pcm_uframes_t period_frames() const;
    snd_pcm_uframes_t buffer_frames() const;
    snd_pcm_format_t pcm_container_format() const;

private:
    snd_pcm_t* handle_ = nullptr;
    AudioFormat format_{};
    snd_pcm_uframes_t period_frames_ = 588;
    snd_pcm_uframes_t buffer_frames_ = 2352;
    snd_pcm_format_t pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
    bool simd_conversion_enabled_ = false;
    std::atomic<std::uint64_t> simd_conversion_samples_processed_{0};
    std::vector<std::int16_t> temp16_;
    std::vector<std::int32_t> temp32_;
    std::vector<unsigned char> temp24_;
};

} // namespace pcmtp
