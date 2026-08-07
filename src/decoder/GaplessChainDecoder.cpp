// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/decoder/GaplessChainDecoder.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/decoder/RangeLimitedDecoder.hpp"
#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

constexpr std::uint64_t kMinimumPrepareThresholdFrames = 32768;
constexpr std::uint64_t kNativePrepareSeconds = 2;
constexpr std::uint64_t kExternalPrepareSeconds = 5;
constexpr std::uint64_t kNativePrefetchMillis = 500;
constexpr std::uint64_t kExternalPrefetchMillis = 1000;

bool same_format(const AudioFormat& a, const AudioFormat& b) {
    return a.sample_rate == b.sample_rate &&
           a.channels == b.channels &&
           a.bits_per_sample == b.bits_per_sample;
}

} // namespace

GaplessChainDecoder::GaplessChainDecoder(std::vector<GaplessTrackSpec> tracks, std::uint64_t first_track_offset)
    : tracks_(std::move(tracks)), first_track_offset_(first_track_offset) {
    if (tracks_.empty()) {
        throw std::invalid_argument("GaplessChainDecoder requires at least one track");
    }
    format_ = tracks_.front().format;
    track_offsets_.reserve(tracks_.size());
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const GaplessTrackSpec& spec = tracks_[i];
        if (!same_format(format_, spec.format)) {
            throw std::invalid_argument("GaplessChainDecoder requires matching output formats");
        }
        if (!spec.start_sample_known ||
            spec.planned_end_sample < spec.start_sample) {
            throw std::invalid_argument(
                "GaplessChainDecoder requires a valid start and planned end");
        }
        if (spec.boundary_mode == GaplessBoundaryMode::ExactRange &&
            !spec.exact_end_sample_known) {
            throw std::invalid_argument(
                "GaplessChainDecoder exact-range track requires an exact end sample");
        }
        track_offsets_.push_back(total_samples_per_channel_);
        const std::uint64_t length = track_length(i);
        if (length > std::numeric_limits<std::uint64_t>::max() -
                         total_samples_per_channel_) {
            throw std::overflow_error(
                "GaplessChainDecoder total sample count overflow");
        }
        total_samples_per_channel_ += length;
    }
    const std::uint64_t first_length = track_length(0);
    if (first_length > 0 && first_track_offset_ > first_length) {
        first_track_offset_ = first_length;
    }
}

GaplessChainDecoder::~GaplessChainDecoder() {
    request_abort();
    close_prepare_thread();
}

std::unique_ptr<IAudioDecoder> GaplessChainDecoder::create_decoder_for_track(const GaplessTrackSpec& spec) const {
    std::unique_ptr<IAudioDecoder> decoder;
    if (spec.native_flac) {
        decoder.reset(new FlacStreamDecoder());
    } else {
        std::unique_ptr<ExternalAudioDecoder> external(new ExternalAudioDecoder(spec.forced_output_sample_rate,
                                                                                spec.forced_output_bits_per_sample,
                                                                                spec.resample_quality,
                                                                                spec.bitdepth_quality));
        if (spec.has_known_external_info) {
            external->set_known_info(spec.known_external_info);
        }
        decoder.reset(external.release());
    }
    if (spec.boundary_mode == GaplessBoundaryMode::ExactRange) {
        Logger::instance().debug("Bounded transport enabled for gapless chain track");
        decoder.reset(new RangeLimitedDecoder(std::move(decoder),
                                              spec.start_sample,
                                              spec.planned_end_sample));
    } else {
        const PresentationEndKind end_kind = decoder->presentation_end_kind();
        if (end_kind != PresentationEndKind::TrustedDecoderEof &&
            end_kind != PresentationEndKind::ExactSampleSpan) {
            throw std::runtime_error(
                "Gapless EOF-bound track does not provide a trusted presentation EOF");
        }
        Logger::instance().debug(
            "Trusted decoder EOF enabled for gapless chain track");
    }
    return decoder;
}

void GaplessChainDecoder::open_decoder_at_local_offset(
    IAudioDecoder& decoder,
    const GaplessTrackSpec& spec,
    std::uint64_t local_offset) const {
    if (spec.boundary_mode == GaplessBoundaryMode::ExactRange) {
        decoder.open_at_sample(spec.path, local_offset);
        return;
    }
    const std::uint64_t source_start = spec.start_sample_known ? spec.start_sample : 0;
    const std::uint64_t source_offset = local_offset >
            std::numeric_limits<std::uint64_t>::max() - source_start
        ? std::numeric_limits<std::uint64_t>::max()
        : source_start + local_offset;
    decoder.open_at_sample(spec.path, source_offset);
}

void GaplessChainDecoder::open_current_decoder(std::uint64_t offset) {
    std::unique_ptr<IAudioDecoder> decoder =
        create_decoder_for_track(tracks_[current_index_]);
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        current_decoder_ = std::move(decoder);
        if (abort_requested_.load(std::memory_order_acquire)) {
            current_decoder_->request_abort();
        }
    }
    open_decoder_at_local_offset(*current_decoder_, tracks_[current_index_], offset);
    current_track_position_ = offset;
    Logger::instance().debug("GaplessChainDecoder opened track index=" + std::to_string(current_index_) +
                             " offset=" + std::to_string(offset) +
                             " path=" + tracks_[current_index_].path);
}

void GaplessChainDecoder::open(const std::string&) {
    open_at_sample(std::string(), first_track_offset_);
}

void GaplessChainDecoder::open_at_sample(const std::string&, std::uint64_t sample_index) {
    close_prepare_thread();
    abort_requested_.store(false, std::memory_order_release);
    requested_end_sample_.store(std::numeric_limits<std::uint64_t>::max(),
                                std::memory_order_relaxed);
    requested_last_segment_index_.store(
        std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed);
    current_index_ = 0;
    current_track_position_ = 0;
    reached_eof_ = false;
    opened_ = false;
    open_current_decoder(std::min<std::uint64_t>(sample_index, track_length(0)));
    opened_ = true;
    maybe_prepare_next();
}

const AudioFormat& GaplessChainDecoder::format() const {
    return format_;
}

std::size_t GaplessChainDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_ || !current_decoder_) {
        throw std::runtime_error("GaplessChainDecoder not opened");
    }
    if (max_samples == 0 || reached_eof_) {
        return 0;
    }

    const std::uint16_t channels = std::max<std::uint16_t>(1, format_.channels);
    max_samples -= max_samples % channels;
    if (max_samples == 0) {
        return 0;
    }
    std::size_t copied = 0;
    while (copied < max_samples && !reached_eof_) {
        if (requested_segment_end_reached()) {
            reached_eof_ = true;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(prepare_mutex_);
            if (current_prebuffer_offset_ < current_prebuffer_.size()) {
                const std::size_t available =
                    current_prebuffer_.size() - current_prebuffer_offset_;
                if (current_prebuffer_offset_ % channels != 0 ||
                    available % channels != 0) {
                    throw std::runtime_error(
                        "Gapless prebuffer contains an incomplete PCM frame");
                }
                std::size_t take = std::min(available, max_samples - copied);
                const std::uint64_t requested_end =
                    requested_end_sample_.load(std::memory_order_relaxed);
                if (requested_end != std::numeric_limits<std::uint64_t>::max()) {
                    const std::uint64_t chain_position = current_chain_position();
                    const std::uint64_t remaining_frames = requested_end > chain_position
                        ? (requested_end - chain_position)
                        : 0;
                    take = static_cast<std::size_t>(std::min<std::uint64_t>(
                        static_cast<std::uint64_t>(take),
                        remaining_frames * channels));
                }
                take -= take % channels;
                std::copy(current_prebuffer_.data() + current_prebuffer_offset_,
                          current_prebuffer_.data() + current_prebuffer_offset_ + take,
                          destination + copied);
                current_prebuffer_offset_ += take;
                copied += take;
                current_track_position_ += static_cast<std::uint64_t>(take / channels);
                if (current_prebuffer_offset_ >= current_prebuffer_.size()) {
                    current_prebuffer_.clear();
                    current_prebuffer_offset_ = 0;
                }
                if (copied >= max_samples) {
                    break;
                }
            }
        }

        const std::uint64_t length = track_length(current_index_);
        const bool range_limited = track_uses_exact_range(current_index_);
        if ((range_limited && current_track_position_ >= length) || current_decoder_->eof()) {
            if (requested_segment_end_reached()) {
                reached_eof_ = true;
                break;
            }
            const SwitchResult sw = stop_requested_after_current_segment()
                ? SwitchResult::NoNext
                : switch_to_next_track();
            if (sw == SwitchResult::Switched) {
                continue;
            }
            reached_eof_ = true;
            break;
        }

        std::size_t request_samples = max_samples - copied;
        if (range_limited) {
            const std::uint64_t remaining_frames = length > current_track_position_ ? (length - current_track_position_) : 0;
            request_samples = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining_frames * channels,
                static_cast<std::uint64_t>(request_samples)));
        }
        const std::uint64_t requested_end =
            requested_end_sample_.load(std::memory_order_relaxed);
        if (requested_end != std::numeric_limits<std::uint64_t>::max()) {
            const std::uint64_t chain_position = current_chain_position();
            const std::uint64_t remaining_frames = requested_end > chain_position
                ? (requested_end - chain_position)
                : 0;
            request_samples = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining_frames * channels,
                static_cast<std::uint64_t>(request_samples)));
        }
        request_samples -= request_samples % channels;
        if (request_samples == 0) {
            if (requested_segment_end_reached()) {
                reached_eof_ = true;
                break;
            }
            const SwitchResult sw = stop_requested_after_current_segment()
                ? SwitchResult::NoNext
                : switch_to_next_track();
            if (sw == SwitchResult::Switched) {
                continue;
            }
            reached_eof_ = true;
            break;
        }

        const std::size_t got = current_decoder_->read_samples(destination + copied, request_samples);
        if (got > request_samples) {
            throw std::runtime_error("Decoder returned more PCM samples than requested");
        }
        if (got % channels != 0) {
            throw std::runtime_error("Decoder returned an incomplete PCM frame");
        }
        if (got == 0) {
            if (requested_segment_end_reached()) {
                reached_eof_ = true;
                break;
            }
            const SwitchResult sw = stop_requested_after_current_segment()
                ? SwitchResult::NoNext
                : switch_to_next_track();
            if (sw == SwitchResult::Switched) {
                continue;
            }
            reached_eof_ = true;
            break;
        }
        copied += got;
        current_track_position_ += static_cast<std::uint64_t>(got / channels);
        maybe_prepare_next();
    }
    return copied;
}

bool GaplessChainDecoder::eof() const {
    return reached_eof_;
}

std::uint64_t GaplessChainDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string GaplessChainDecoder::source_path() const {
    if (current_index_ < tracks_.size()) {
        return tracks_[current_index_].path;
    }
    return std::string();
}

PresentationEndKind GaplessChainDecoder::presentation_end_kind() const noexcept {
    return uses_decoder_eof_boundaries()
        ? PresentationEndKind::TrustedDecoderEof
        : PresentationEndKind::ExactSampleSpan;
}

DecoderSegmentPosition GaplessChainDecoder::segment_position() const noexcept {
    DecoderSegmentPosition position;
    position.valid = opened_;
    position.index = current_index_;
    position.samples_per_channel = current_track_position_;
    return position;
}

TransportTruncationKind GaplessChainDecoder::transport_truncation_kind() const noexcept {
    return TransportTruncationKind::DecoderSegmentBoundary;
}

bool GaplessChainDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_) {
        return false;
    }
    // Estimated EOF-bound offsets are suitable for display and prefetch only.
    // Cross-segment seeking must not use them as exact transport coordinates.
    if (uses_decoder_eof_boundaries()) {
        return false;
    }
    close_prepare_thread();
    abort_requested_.store(false, std::memory_order_release);
    reached_eof_ = false;
    std::uint64_t pos = sample_index;
    current_index_ = 0;
    while (current_index_ + 1 < tracks_.size()) {
        const std::uint64_t length = track_length(current_index_);
        if (pos < length) {
            break;
        }
        pos -= length;
        ++current_index_;
    }
    open_current_decoder(pos);
    maybe_prepare_next();
    return true;
}

std::uint64_t GaplessChainDecoder::current_chain_position() const {
    if (current_index_ >= track_offsets_.size()) {
        return total_samples_per_channel_;
    }
    return track_offsets_[current_index_] + current_track_position_;
}

bool GaplessChainDecoder::requested_segment_end_reached() const {
    const std::uint64_t requested_end =
        requested_end_sample_.load(std::memory_order_relaxed);
    return requested_end != std::numeric_limits<std::uint64_t>::max() &&
           current_chain_position() >= requested_end;
}

void GaplessChainDecoder::request_abort() {
    abort_requested_.store(true, std::memory_order_release);
    prepare_cancel_requested_.store(true, std::memory_order_release);
    prepare_generation_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (current_decoder_ != nullptr) {
            current_decoder_->request_abort();
        }
    }
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (preparing_decoder_ != nullptr) {
            preparing_decoder_->request_abort();
        }
        if (prepared_.decoder != nullptr) {
            prepared_.decoder->request_abort();
        }
    }
}

void GaplessChainDecoder::request_stop_after_current_segment(
    std::uint64_t segment_end_sample) {
    // A numerical chain coordinate is exact only when every segment has an
    // exact range. EOF-bound chains use request_stop_after_segment().
    if (uses_decoder_eof_boundaries()) {
        return;
    }
    const std::uint64_t clamped =
        std::min(segment_end_sample, total_samples_per_channel_);
    std::uint64_t current = requested_end_sample_.load(std::memory_order_relaxed);
    while (clamped < current &&
           !requested_end_sample_.compare_exchange_weak(current,
                                                        clamped,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
    }
}

void GaplessChainDecoder::request_stop_after_segment(std::size_t segment_index) {
    const std::size_t clamped = std::min(segment_index, tracks_.size() - 1);
    std::size_t current =
        requested_last_segment_index_.load(std::memory_order_relaxed);
    while (clamped < current &&
           !requested_last_segment_index_.compare_exchange_weak(
               current,
               clamped,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void GaplessChainDecoder::close_prepare_thread() {
    prepare_cancel_requested_.store(true, std::memory_order_release);
    prepare_generation_.fetch_add(1, std::memory_order_acq_rel);

    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (preparing_decoder_ != nullptr) {
            preparing_decoder_->request_abort();
        }
        if (prepared_.decoder != nullptr) {
            prepared_.decoder->request_abort();
        }
        if (prepare_thread_.joinable()) {
            thread_to_join = std::move(prepare_thread_);
        }
    }
    if (thread_to_join.joinable()) {
        thread_to_join.join();
    }

    std::lock_guard<std::mutex> lock(prepare_mutex_);
    preparing_decoder_ = nullptr;
    preparing_ = false;
    prepared_ = PreparedNext{};
    current_prebuffer_.clear();
    current_prebuffer_offset_ = 0;
}

void GaplessChainDecoder::maybe_prepare_next() {
    if (abort_requested_.load(std::memory_order_acquire) ||
        current_index_ + 1 >= tracks_.size()) {
        return;
    }
    const std::size_t next_index = current_index_ + 1;
    const std::size_t requested_last =
        requested_last_segment_index_.load(std::memory_order_relaxed);
    if (requested_last != std::numeric_limits<std::size_t>::max() &&
        next_index > requested_last) {
        return;
    }
    const std::uint64_t requested_end =
        requested_end_sample_.load(std::memory_order_relaxed);
    if (requested_end != std::numeric_limits<std::uint64_t>::max() &&
        track_offsets_[next_index] >= requested_end) {
        return;
    }
    const std::uint64_t length = track_length(current_index_);
    const std::uint64_t threshold = prepare_threshold_frames(next_index);
    if (current_decoder_ != nullptr && !current_decoder_->eof() &&
        length > current_track_position_ &&
        (length - current_track_position_) > threshold) {
        return;
    }
    std::thread completed_thread;
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (preparing_ || (prepared_.ready && prepared_.index == next_index)) {
            return;
        }
        if (prepare_thread_.joinable()) {
            completed_thread = std::move(prepare_thread_);
        }

        const std::uint64_t generation =
            prepare_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        prepare_cancel_requested_.store(false, std::memory_order_release);
        preparing_ = true;
        prepare_thread_ = std::thread(
            &GaplessChainDecoder::prepare_next_worker,
            this,
            next_index,
            generation);
    }
    if (completed_thread.joinable()) {
        completed_thread.join();
    }
}

void GaplessChainDecoder::prepare_next_worker(std::size_t index,
                                               std::uint64_t generation) {
    const auto cancelled = [this, generation]() {
        return abort_requested_.load(std::memory_order_acquire) ||
               prepare_cancel_requested_.load(std::memory_order_acquire) ||
               prepare_generation_.load(std::memory_order_acquire) != generation;
    };

    PreparedNext prepared;
    prepared.index = index;
    try {
        if (!cancelled()) {
            prepared.decoder = create_decoder_for_track(tracks_[index]);
        }
        if (prepared.decoder != nullptr) {
            std::lock_guard<std::mutex> lock(prepare_mutex_);
            if (cancelled()) {
                prepared.decoder->request_abort();
            } else {
                preparing_decoder_ = prepared.decoder.get();
            }
        }
        if (!cancelled() && prepared.decoder != nullptr) {
            open_decoder_at_local_offset(*prepared.decoder, tracks_[index], 0);
        }
        if (!cancelled() && prepared.decoder != nullptr) {
            prepared.prebuffer.resize(prebuffer_samples(index));
            const std::size_t got = prepared.decoder->read_samples(
                prepared.prebuffer.data(), prepared.prebuffer.size());
            prepared.prebuffer.resize(got);
            if (!cancelled()) {
                prepared.ready = true;
                Logger::instance().debug(
                    "GaplessChainDecoder prebuffered next track index=" +
                    std::to_string(index) + " samples=" + std::to_string(got));
            }
        }
    } catch (const std::exception& ex) {
        prepared.failed = true;
        if (!cancelled()) {
            Logger::instance().error(
                std::string("GaplessChainDecoder failed to prebuffer next track: ") +
                ex.what());
        }
    }

    std::lock_guard<std::mutex> lock(prepare_mutex_);
    if (preparing_decoder_ == prepared.decoder.get()) {
        preparing_decoder_ = nullptr;
    }
    if (!cancelled()) {
        prepared_ = std::move(prepared);
    }
    if (prepare_generation_.load(std::memory_order_acquire) == generation) {
        preparing_ = false;
    }
}

GaplessChainDecoder::SwitchResult GaplessChainDecoder::switch_to_next_track() {
    if (current_index_ + 1 >= tracks_.size() ||
        stop_requested_after_current_segment()) {
        return SwitchResult::NoNext;
    }

    const std::size_t next_index = current_index_ + 1;
    bool still_preparing = false;
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        still_preparing = preparing_;
    }

    if (prepare_thread_.joinable()) {
        if (still_preparing) {
            Logger::instance().debug(
                "Gapless prebuffer was not ready at the track boundary; "
                "waiting for preparation to finish");
        }
        prepare_thread_.join();
    }

    if (abort_requested_.load(std::memory_order_acquire)) {
        return SwitchResult::NoNext;
    }

    bool switched_to_prebuffered = false;
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (prepared_.ready && prepared_.index == next_index && prepared_.decoder) {
            {
                std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
                current_decoder_ = std::move(prepared_.decoder);
                if (abort_requested_.load(std::memory_order_acquire)) {
                    current_decoder_->request_abort();
                }
            }
            current_prebuffer_ = std::move(prepared_.prebuffer);
            current_prebuffer_offset_ = 0;
            prepared_ = PreparedNext{};
            current_index_ = next_index;
            current_track_position_ = 0;
            switched_to_prebuffered = true;
        } else if (prepared_.failed && prepared_.index == next_index) {
            Logger::instance().debug(
                "GaplessChainDecoder prebuffer failed; opening next track synchronously");
        }
    }

    if (switched_to_prebuffered) {
        Logger::instance().debug(
            "GaplessChainDecoder switched to prebuffered track index=" +
            std::to_string(current_index_));
        maybe_prepare_next();
        return SwitchResult::Switched;
    }

    current_index_ = next_index;
    open_current_decoder(0);
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        prepared_ = PreparedNext{};
        current_prebuffer_.clear();
        current_prebuffer_offset_ = 0;
        preparing_ = false;
    }
    maybe_prepare_next();
    return SwitchResult::Switched;
}

std::uint64_t GaplessChainDecoder::track_length(std::size_t index) const {
    if (index >= tracks_.size()) {
        return 0;
    }
    const GaplessTrackSpec& spec = tracks_[index];
    if (!spec.start_sample_known ||
        spec.planned_end_sample < spec.start_sample) {
        return 0;
    }
    return spec.planned_end_sample - spec.start_sample;
}

bool GaplessChainDecoder::track_uses_exact_range(std::size_t index) const {
    return index < tracks_.size() &&
           tracks_[index].boundary_mode == GaplessBoundaryMode::ExactRange;
}

bool GaplessChainDecoder::uses_decoder_eof_boundaries() const noexcept {
    return std::any_of(tracks_.begin(), tracks_.end(), [](const GaplessTrackSpec& track) {
        return track.boundary_mode == GaplessBoundaryMode::DecoderEof;
    });
}

bool GaplessChainDecoder::stop_requested_after_current_segment() const {
    const std::size_t requested_last =
        requested_last_segment_index_.load(std::memory_order_relaxed);
    return requested_last != std::numeric_limits<std::size_t>::max() &&
           current_index_ >= requested_last;
}

std::uint64_t GaplessChainDecoder::prepare_threshold_frames(std::size_t index) const {
    if (index >= tracks_.size()) {
        return kMinimumPrepareThresholdFrames;
    }
    const GaplessTrackSpec& spec = tracks_[index];
    const std::uint32_t rate = std::max<std::uint32_t>(1, spec.format.sample_rate);
    const std::uint64_t seconds =
        spec.native_flac ? kNativePrepareSeconds : kExternalPrepareSeconds;
    return std::max<std::uint64_t>(kMinimumPrepareThresholdFrames, static_cast<std::uint64_t>(rate) * seconds);
}

std::size_t GaplessChainDecoder::prebuffer_samples(std::size_t index) const {
    if (index >= tracks_.size()) {
        return 16384;
    }
    const GaplessTrackSpec& spec = tracks_[index];
    const std::uint32_t rate = std::max<std::uint32_t>(1, spec.format.sample_rate);
    const std::uint16_t channels = std::max<std::uint16_t>(1, spec.format.channels);
    const std::uint64_t millis =
        spec.native_flac ? kNativePrefetchMillis : kExternalPrefetchMillis;
    const std::uint64_t frames = std::max<std::uint64_t>(8192, (static_cast<std::uint64_t>(rate) * millis) / 1000);
    const std::uint64_t samples = frames * static_cast<std::uint64_t>(channels);
    const std::uint64_t capped_samples = std::min<std::uint64_t>(
        samples, static_cast<std::uint64_t>(1024 * 1024));
    return static_cast<std::size_t>(
        (capped_samples / channels) * channels);
}

} // namespace pcmtp
