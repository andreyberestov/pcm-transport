#include "pcmtp/decoder/FlacStreamDecoder.hpp"

#include <FLAC/metadata.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {

namespace {

std::string to_upper_ascii(std::string value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
    }
    return value;
}

} // namespace

FlacStreamDecoder::~FlacStreamDecoder() {
    if (opened_) {
        finish();
    }
}

void FlacStreamDecoder::open(const std::string& path) {
    if (opened_) {
        finish();
    }

    path_ = path;
    queue_.clear();
    reached_eof_ = false;
    metadata_seen_ = false;
    total_samples_per_channel_ = 0;

    const auto init_status = init(path.c_str());
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        throw std::runtime_error("FLAC init failed");
    }

    if (!process_until_end_of_metadata()) {
        throw std::runtime_error("Failed to read FLAC metadata");
    }

    if (!metadata_seen_) {
        throw std::runtime_error("FLAC metadata not available");
    }

    opened_ = true;
    Logger::instance().info("Opened FLAC stream: " + path);
}

const AudioFormat& FlacStreamDecoder::format() const {
    return format_;
}

std::size_t FlacStreamDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_) {
        throw std::runtime_error("Decoder not opened");
    }

    fill_queue_if_needed();

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = std::min(max_samples, queue_.size());
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] = queue_.front();
        queue_.pop_front();
    }
    return count;
}

bool FlacStreamDecoder::eof() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reached_eof_ && queue_.empty();
}

std::uint64_t FlacStreamDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string FlacStreamDecoder::source_path() const {
    return path_;
}

bool FlacStreamDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        reached_eof_ = false;
    }
    return seek_absolute(sample_index);
}

FlacTags FlacStreamDecoder::read_tags(const std::string& path) {
    FlacTags tags;
    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (chain == nullptr) {
        return tags;
    }

    if (!FLAC__metadata_chain_read(chain, path.c_str())) {
        FLAC__metadata_chain_delete(chain);
        return tags;
    }

    FLAC__Metadata_Iterator* iterator = FLAC__metadata_iterator_new();
    if (iterator == nullptr) {
        FLAC__metadata_chain_delete(chain);
        return tags;
    }

    FLAC__metadata_iterator_init(iterator, chain);
    do {
        FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(iterator);
        if (block != nullptr && block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            const FLAC__StreamMetadata_VorbisComment& vc = block->data.vorbis_comment;
            for (unsigned i = 0; i < vc.num_comments; ++i) {
                const FLAC__StreamMetadata_VorbisComment_Entry& entry = vc.comments[i];
                const std::string text(reinterpret_cast<const char*>(entry.entry), entry.length);
                const std::size_t eq = text.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const std::string key = to_upper_ascii(text.substr(0, eq));
                const std::string value = text.substr(eq + 1);
                if (key == "TITLE") {
                    tags.title = value;
                } else if (key == "ARTIST") {
                    tags.artist = value;
                } else if (key == "TRACKNUMBER") {
                    try {
                        tags.track_number = std::stoi(value);
                    } catch (...) {
                    }
                }
            }
        }
    } while (FLAC__metadata_iterator_next(iterator));

    FLAC__metadata_iterator_delete(iterator);
    FLAC__metadata_chain_delete(chain);
    return tags;
}

::FLAC__StreamDecoderWriteStatus FlacStreamDecoder::write_callback(const ::FLAC__Frame* frame,
                                                                   const ::FLAC__int32* const buffer[]) {
    const unsigned blocksize = frame->header.blocksize;
    std::lock_guard<std::mutex> lock(mutex_);
    for (unsigned i = 0; i < blocksize; ++i) {
        for (unsigned ch = 0; ch < format_.channels; ++ch) {
            queue_.push_back(static_cast<PcmSample>(buffer[ch][i]));
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacStreamDecoder::metadata_callback(const ::FLAC__StreamMetadata* metadata) {
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }

    format_.sample_rate = metadata->data.stream_info.sample_rate;
    format_.channels = metadata->data.stream_info.channels;
    format_.bits_per_sample = static_cast<std::uint16_t>(metadata->data.stream_info.bits_per_sample);
    total_samples_per_channel_ = metadata->data.stream_info.total_samples;
    metadata_seen_ = true;
}

void FlacStreamDecoder::error_callback(::FLAC__StreamDecoderErrorStatus status) {
    const std::string message = FLAC__StreamDecoderErrorStatusString[status];
    Logger::instance().error("FLAC decoder error: " + message);
    std::cerr << "FLAC decoder error: " << message << '\n';
}

void FlacStreamDecoder::fill_queue_if_needed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reached_eof_ || queue_.size() >= 8192) {
            return;
        }
    }

    Logger::instance().debug("FLAC queue low, decoding next frame");

    if (!process_single()) {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_eof_ = true;
        Logger::instance().debug("FLAC process_single returned false, marking EOF");
        return;
    }

    const auto state = get_state();
    if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_eof_ = true;
        Logger::instance().debug("FLAC decoder reached end of stream");
    }
}

} // namespace pcmtp
