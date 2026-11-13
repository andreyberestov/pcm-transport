// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <alsa/asoundlib.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "pcmtp/backend/IAudioBackend.hpp"

namespace pcmtp {

enum class Alsa24BitContainerMode {
    Auto,
    S24LE,
    S24_3LE,
    S32LE
};

struct AlsaProbeCell {
    std::string format_name;
    unsigned sample_rate = 0;
    bool supported = false;
};

struct AlsaProbeMatrix {
    std::string device_name;
    std::string plug_device_name;
    std::vector<unsigned> sample_rates;
    std::vector<std::string> format_names;
    std::vector<AlsaProbeCell> cells;
    std::vector<bool> plug_float32_supported;
    std::vector<bool> plug_float64_supported;
};

class AlsaPcmBackend final : public IAudioBackend {
public:
    AlsaPcmBackend() = default;
    ~AlsaPcmBackend() override;

    void open(const std::string& device_name,
              const AudioFormat& format,
              PcmOutputFormat output_format) override;
    std::size_t write_samples(const PcmBlock& samples,
                              std::size_t offset,
                              std::size_t sample_count) override;
    bool write_start_silence_chunk() override;
    bool pause_device() override;
    void resume_device() override;
    void resume_software_pause() override;
    void log_transport_state(const char* event) override;
    void drain() override;
    void close() override;
    std::string active_output_report() const override;

    void set_24bit_container_mode(Alsa24BitContainerMode mode);
    void set_start_silence_ms(unsigned milliseconds);

    static AlsaProbeMatrix probe_device_format_matrix(
        const std::string& device_name,
        const std::string& plug_device_name = std::string());

private:
    snd_pcm_t* handle_ = nullptr;
    AudioFormat format_{};
    PcmOutputFormat output_format_ = PcmOutputFormat::S16;
    snd_pcm_uframes_t period_frames_ = 588;
    snd_pcm_uframes_t buffer_frames_ = 2352;
    snd_pcm_format_t pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
    Alsa24BitContainerMode format_24bit_mode_ = Alsa24BitContainerMode::Auto;
    std::string device_name_;
    unsigned accepted_sample_rate_ = 0;
    int active_significant_bits_ = 0;
    std::vector<std::int32_t> scratch_s32_;
    std::vector<std::int16_t> scratch_s16_;
    std::vector<unsigned char> scratch_s24_;
    std::vector<unsigned char> scratch_start_silence_;
    unsigned start_silence_ms_ = 0;
    std::uint64_t start_silence_total_frames_ = 0;
    std::uint64_t start_silence_remaining_frames_ = 0;
    bool start_silence_started_ = false;
    unsigned start_silence_recoveries_ = 0;
    bool hw_pause_supported_ = false;
    bool hw_pause_active_ = false;
    bool software_resume_check_pending_ = false;
    bool diagnostic_enabled_ = false;
    std::uint64_t diagnostic_session_id_ = 0;
    bool diagnostic_first_write_attempted_ = false;
    bool diagnostic_first_write_succeeded_ = false;
    std::uint64_t diagnostic_epipe_count_ = 0;
    std::uint64_t diagnostic_suspend_count_ = 0;
    std::chrono::steady_clock::time_point diagnostic_open_started_at_{};
    std::chrono::steady_clock::time_point diagnostic_open_ready_at_{};
};

} // namespace pcmtp
