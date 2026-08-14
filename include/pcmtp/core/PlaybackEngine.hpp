// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>

#include "pcmtp/backend/IAudioBackend.hpp"
#include "pcmtp/decoder/IAudioDecoder.hpp"
#include "pcmtp/dsp/ToneControlDesign.hpp"

namespace pcmtp {

struct PlaybackStatusSnapshot {
    bool playing = false;
    bool paused = false;
    bool finished = false;
    AudioFormat format{};
    std::uint64_t current_samples_per_channel = 0;
    std::uint64_t total_samples_per_channel = 0;
    bool segment_position_valid = false;
    std::size_t segment_index = 0;
    std::uint64_t segment_samples_per_channel = 0;
    TransportTruncationKind transport_truncation_kind =
        TransportTruncationKind::None;
    std::string message;
    std::string active_output_report;
};

struct PlaybackTransportSnapshot {
    bool playing = false;
    bool paused = false;
    bool finished = false;
    AudioFormat format{};
    std::uint64_t current_samples_per_channel = 0;
    std::uint64_t total_samples_per_channel = 0;
    bool segment_position_valid = false;
    std::size_t segment_index = 0;
    std::uint64_t segment_samples_per_channel = 0;
    TransportTruncationKind transport_truncation_kind =
        TransportTruncationKind::None;
};

struct PlaybackMeterSnapshot {
    bool peak_measured = false;
    float peak_level = 0.0f;
    std::uint32_t clipped_samples = 0;
    bool transport_active = false;
};

enum class PlaybackEventKind {
    SegmentChanged,
    ProcessingStateChanged,
    Finished,
    Error
};

struct PlaybackEvent {
    PlaybackEventKind kind = PlaybackEventKind::Finished;
    std::uint64_t transport_generation = 0;
};

class PlaybackEngine {
public:
    PlaybackEngine();
    ~PlaybackEngine();

    void start(std::unique_ptr<IAudioDecoder> decoder,
               std::unique_ptr<IAudioBackend> backend,
               const std::string& device_name,
               std::uint64_t initial_samples_per_channel = 0,
               std::vector<std::uint64_t> logical_segment_offsets = {});

    void stop();
    void pause();
    void resume();
    void request_stop_after_current_segment(std::uint64_t segment_end_sample);
    void request_stop_after_segment(std::size_t segment_index);

    bool is_playing() const;
    bool is_paused() const;
    void set_soft_volume_percent(int percent);
    int soft_volume_percent() const;
    void set_soft_eq(int bass_db, int treble_db);
    void set_pre_eq_headroom_tenths_db(int tenths_db);
    int pre_eq_headroom_tenths_db() const;
    void set_soft_eq_profile(int bass_hz, int treble_hz);
    void set_deep_bass_enabled(bool enabled);
    bool deep_bass_enabled() const;
    void set_deep_bass_preset(int preset);
    void set_deep_bass_amount(int amount_steps);
    void set_level_meter_enabled(bool enabled);
    void set_clip_detection_enabled(bool enabled);
    int bass_db() const;
    int treble_db() const;
    ResamplerRuntimeKind resampler_runtime_kind() const noexcept;
    PlaybackStatusSnapshot snapshot() const;
    PlaybackTransportSnapshot transport_snapshot() const;
    PlaybackMeterSnapshot consume_meter_snapshot();

    int playback_event_fd() const noexcept;
    std::size_t drain_playback_events(
        std::array<PlaybackEvent, 4>& events) noexcept;
    bool consume_finished_transport();
    std::uint64_t transport_generation() const noexcept;

    void set_realtime_priority_enabled(bool enabled);
    void set_realtime_priority(int priority);
    std::string refresh_realtime_priority_status();
    std::string request_realtime_priority_for_playback_thread();

    std::string active_output_report() const;

private:
    struct LiveTransportPosition {
        std::uint64_t current_samples_per_channel = 0;
        bool segment_position_valid = false;
        std::size_t segment_index = 0;
        std::uint64_t segment_samples_per_channel = 0;
    };

    void playback_loop(std::uint64_t transport_generation);
    void publish_live_transport_position(
        std::uint64_t current_samples_per_channel,
        const DecoderSegmentPosition& segment) noexcept;
    LiveTransportPosition read_live_transport_position() const noexcept;
    void emit_playback_event(PlaybackEventKind kind,
                             std::uint64_t transport_generation) noexcept;
    void clear_pending_playback_events() noexcept;
    std::string try_set_realtime_priority_for_current_thread();
    std::string verified_realtime_priority_status(long tid) const;
    void wait_if_paused();
    void set_error(const std::string& message);
    void join_threads();

    mutable std::mutex state_mutex_;
    // Discrete transport state lives under state_mutex_.  The position fields
    // are maintained at lifecycle transitions only; snapshot accessors always
    // overlay the coherent live-position tuple published below.
    PlaybackStatusSnapshot snapshot_{};
    std::string last_error_;

    // The playback thread is the only writer while a transport is active.
    // start()/stop() publish only before the thread starts or after it joins.
    // The sequence counter makes the four atomic fields below one coherent
    // live-position tuple without placing state_mutex_ in the realtime path.
    std::atomic<std::uint64_t> live_position_sequence_{0};
    std::atomic<std::uint64_t> live_current_samples_per_channel_{0};
    std::atomic<bool> live_segment_position_valid_{false};
    std::atomic<std::size_t> live_segment_index_{0};
    std::atomic<std::uint64_t> live_segment_samples_per_channel_{0};
    std::atomic<ResamplerRuntimeKind> resampler_runtime_kind_{
        ResamplerRuntimeKind::NotUsed};

    std::unique_ptr<IAudioDecoder> decoder_;
    std::unique_ptr<IAudioBackend> backend_;
    AudioFormat format_{};
    std::string device_name_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};
    std::thread playback_thread_;
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    std::uint64_t initial_samples_per_channel_ = 0;
    std::atomic<int> soft_volume_percent_{100};
    std::atomic<int> bass_db_{0};
    std::atomic<int> treble_db_{0};
    std::atomic<int> pre_eq_headroom_tenths_db_{0};
    std::atomic<int> bass_hz_{110};
    std::atomic<int> treble_hz_{10000};
    std::atomic<bool> deep_bass_enabled_{false};
    std::atomic<int> deep_bass_preset_{static_cast<int>(tone::DeepBassPreset::Focused)};
    std::atomic<int> deep_bass_amount_{0};
    std::atomic<bool> level_meter_enabled_{true};
    std::atomic<bool> clip_detection_enabled_{true};
    // Valid values are Q24 peak magnitudes; all-bits-one means that no PCM
    // measurement has been published since the previous GUI consume.
    std::atomic<std::uint32_t> level_meter_peak_units_{~std::uint32_t{0}};
    std::atomic<std::uint32_t> clipped_samples_pending_{0};
    std::atomic<bool> meter_transport_active_{false};
    std::atomic<bool> realtime_priority_enabled_{false};
    std::atomic<int> realtime_priority_{60};
    std::atomic<long> playback_thread_tid_{0};
    std::atomic<std::uint64_t> transport_generation_{0};
    int playback_event_fd_ = -1;
    std::atomic<std::uint32_t> pending_playback_event_bits_{0};
    std::atomic<std::uint64_t> pending_segment_generation_{0};
    std::atomic<std::uint64_t> pending_processing_state_generation_{0};
    std::atomic<std::uint64_t> pending_finished_generation_{0};
    std::atomic<std::uint64_t> pending_error_generation_{0};
    std::vector<std::uint64_t> logical_segment_offsets_;
    mutable std::mutex runtime_mutex_;
    std::string realtime_priority_status_ = "Realtime priority: disabled";
    std::string last_realtime_priority_error_;
    std::string last_active_output_report_;
};

} // namespace pcmtp
