#pragma once

#include <FLAC++/decoder.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

struct FlacTags {
    std::string title;
    std::string artist;
    int track_number = 0;
};

class FlacStreamDecoder final : public IAudioDecoder, private FLAC::Decoder::File {
public:
    FlacStreamDecoder() = default;
    ~FlacStreamDecoder() override;

    void open(const std::string& path) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;

    static FlacTags read_tags(const std::string& path);

private:
    ::FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame* frame,
                                                    const ::FLAC__int32* const buffer[]) override;
    void metadata_callback(const ::FLAC__StreamMetadata* metadata) override;
    void error_callback(::FLAC__StreamDecoderErrorStatus status) override;

    void fill_queue_if_needed();

    AudioFormat format_{};
    bool opened_ = false;
    bool reached_eof_ = false;
    bool metadata_seen_ = false;
    std::deque<PcmSample> queue_;
    mutable std::mutex mutex_;
    std::string path_;
    std::uint64_t total_samples_per_channel_ = 0;
};

} // namespace pcmtp
