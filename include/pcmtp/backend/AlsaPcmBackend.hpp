#pragma once

#include <alsa/asoundlib.h>

#include <string>

#include "pcmtp/backend/IAudioBackend.hpp"

namespace pcmtp {

class AlsaPcmBackend final : public IAudioBackend {
public:
    AlsaPcmBackend() = default;
    ~AlsaPcmBackend() override;

    void open(const std::string& device_name, const AudioFormat& format) override;
    std::size_t write_samples(const PcmSample* samples, std::size_t sample_count) override;
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
};

} // namespace pcmtp
