#include "pcmtp/decoder/FlacStreamDecoder.hpp"

#include <FLAC/export.h>
#include <FLAC/metadata.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

using SetNumThreadsFn = FLAC__bool (*)(::FLAC__StreamDecoder*, unsigned);

std::atomic<bool> g_flac_threaded_decode_requested{false};
std::atomic<bool> g_flac_threaded_decode_active{false};
std::atomic<bool> g_flac_threaded_decode_supported{false};
std::string g_flac_threaded_decode_status = "libFLAC build version: unknown\nlinked libFLAC: unknown\nThreaded decoder API symbol: not checked in loaded libFLAC\nThreaded decode supported: no\nThreaded decode requested: no\nThreaded decode active: no";
std::mutex g_flac_threaded_decode_status_mutex;

unsigned flac_thread_count() {
    const unsigned hc = std::thread::hardware_concurrency();
    return std::max(2u, std::min(4u, hc == 0 ? 2u : hc));
}

std::string to_upper_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::array<int, 3> parse_semver_triplet(const char* version) {
    std::array<int, 3> out{0, 0, 0};
    if (version == nullptr) {
        return out;
    }
    const char* p = version;
    for (int i = 0; i < 3 && *p != '\0'; ++i) {
        while (*p != '\0' && !std::isdigit(static_cast<unsigned char>(*p))) {
            ++p;
        }
        while (*p != '\0' && std::isdigit(static_cast<unsigned char>(*p))) {
            out[static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(i)] * 10 + (*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
        }
    }
    return out;
}

bool version_at_least_1_5_0() {
#ifdef FLAC__VERSION_STRING
    const auto ver = parse_semver_triplet(FLAC__VERSION_STRING);
    return (ver[0] > 1) || (ver[0] == 1 && ver[1] > 5) || (ver[0] == 1 && ver[1] == 5 && ver[2] >= 0);
#else
    return false;
#endif
}

SetNumThreadsFn resolve_set_num_threads_fn() {
    static SetNumThreadsFn fn = []() -> SetNumThreadsFn {
        void* sym = dlsym(RTLD_DEFAULT, "FLAC__stream_decoder_set_num_threads");
        if (sym != nullptr) {
            return reinterpret_cast<SetNumThreadsFn>(sym);
        }
        const char* candidates[] = {"libFLAC.so", "libFLAC.so.12", "libFLAC.so.8", nullptr};
        for (const char** name = candidates; *name != nullptr; ++name) {
            void* handle = dlopen(*name, RTLD_LAZY | RTLD_LOCAL);
            if (handle == nullptr) {
                continue;
            }
            sym = dlsym(handle, "FLAC__stream_decoder_set_num_threads");
            dlclose(handle);
            if (sym != nullptr) {
                return reinterpret_cast<SetNumThreadsFn>(sym);
            }
        }
        return nullptr;
    }();
    return fn;
}



std::string flac_version_text() {
#ifdef FLAC__VERSION_STRING
    return FLAC__VERSION_STRING;
#else
    return "unknown";
#endif
}

bool threaded_symbol_available() {
    return resolve_set_num_threads_fn() != nullptr;
}

std::string linked_flac_library_path() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(FLAC__stream_decoder_new), &info) != 0 && info.dli_fname != nullptr) {
        return info.dli_fname;
    }
    return "unknown";
}

std::string make_threaded_status_text(bool requested, bool active, unsigned threads = 0) {
    const bool version_ok = version_at_least_1_5_0();
    const bool symbol_ok = threaded_symbol_available();
    const bool supported = symbol_ok;
    std::ostringstream oss;
    oss << "libFLAC build version: " << flac_version_text() << "\n";
    oss << "linked libFLAC: " << linked_flac_library_path() << "\n";
    oss << "Threaded decoder API symbol: "
        << (symbol_ok ? "found in loaded libFLAC" : "not found in loaded libFLAC")
        << "\n";
    if (!version_ok) {
        oss << "Note: build headers report a pre-1.5.0 FLAC API level\n";
    }
    oss << "Threaded decode supported: " << (supported ? "yes" : "no") << "\n";
    oss << "Threaded decode requested: " << (requested ? "yes" : "no") << "\n";
    oss << "Threaded decode active: " << (active ? "yes" : "no");
    if (active && threads > 0) {
        oss << " (" << threads << " threads)";
    }
    return oss.str();
}

bool compute_threaded_decode_support() {
    return threaded_symbol_available();
}

void set_threaded_runtime_status(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_flac_threaded_decode_status_mutex);
    g_flac_threaded_decode_status = text;
}

std::string get_threaded_runtime_status() {
    std::lock_guard<std::mutex> lock(g_flac_threaded_decode_status_mutex);
    return g_flac_threaded_decode_status;
}

bool try_activate_threaded_decode(::FLAC__StreamDecoder* decoder, unsigned threads) {
    if (decoder == nullptr) {
        return false;
    }
    SetNumThreadsFn fn = resolve_set_num_threads_fn();
    if (fn == nullptr) {
        return false;
    }
    return fn(decoder, threads) != 0;
}

} // namespace

FlacStreamDecoder::~FlacStreamDecoder() {
    reset_decoder();
}

void FlacStreamDecoder::reset_decoder() {
    if (decoder_ != nullptr) {
        FLAC__stream_decoder_finish(decoder_);
        FLAC__stream_decoder_delete(decoder_);
        decoder_ = nullptr;
    }
    opened_ = false;
}

void FlacStreamDecoder::open(const std::string& path) {
    reset_decoder();

    path_ = path;
    blocks_.clear();
    block_offset_ = 0;
    queued_samples_ = 0;
    reached_eof_ = false;
    metadata_seen_ = false;
    total_samples_per_channel_ = 0;
    format_ = AudioFormat{};

    decoder_ = FLAC__stream_decoder_new();
    if (decoder_ == nullptr) {
        throw std::runtime_error("Failed to allocate FLAC decoder");
    }

    g_flac_threaded_decode_active.store(false, std::memory_order_relaxed);
    const bool requested = g_flac_threaded_decode_requested.load(std::memory_order_relaxed);
    const bool supported = compute_threaded_decode_support();
    g_flac_threaded_decode_supported.store(supported, std::memory_order_relaxed);
    if (requested && supported) {
        const unsigned threads = flac_thread_count();
        const bool active = try_activate_threaded_decode(decoder_, threads);
        g_flac_threaded_decode_active.store(active, std::memory_order_relaxed);
        set_threaded_runtime_status(make_threaded_status_text(true, active, active ? threads : 0));
        Logger::instance().info(std::string("FLAC threaded decode requested: ") + std::to_string(threads) + " threads; active=" + (active ? "yes" : "no"));
    } else if (requested && !supported) {
        set_threaded_runtime_status(make_threaded_status_text(true, false, 0));
        Logger::instance().info("FLAC threaded decode requested but decoder threading API is not available in current libFLAC build");
    } else {
        set_threaded_runtime_status(make_threaded_status_text(false, false, 0));
    }

    const auto init_status = FLAC__stream_decoder_init_file(
        decoder_,
        path.c_str(),
        &FlacStreamDecoder::write_callback,
        &FlacStreamDecoder::metadata_callback,
        &FlacStreamDecoder::error_callback,
        this);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        reset_decoder();
        throw std::runtime_error("FLAC init failed");
    }

    if (FLAC__stream_decoder_process_until_end_of_metadata(decoder_) == 0) {
        reset_decoder();
        throw std::runtime_error("Failed to read FLAC metadata");
    }

    if (!metadata_seen_) {
        reset_decoder();
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
    std::size_t copied = 0;
    while (copied < max_samples && !blocks_.empty()) {
        PcmBuffer& front = blocks_.front();
        if (block_offset_ >= front.size()) {
            blocks_.pop_front();
            block_offset_ = 0;
            continue;
        }
        const std::size_t available = front.size() - block_offset_;
        const std::size_t needed = max_samples - copied;
        const std::size_t take = std::min(available, needed);
        std::copy(front.data() + block_offset_, front.data() + block_offset_ + take, destination + copied);
        copied += take;
        block_offset_ += take;
        queued_samples_ -= take;
        if (block_offset_ >= front.size()) {
            blocks_.pop_front();
            block_offset_ = 0;
        }
    }
    return copied;
}

bool FlacStreamDecoder::eof() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reached_eof_ && queued_samples_ == 0;
}

std::uint64_t FlacStreamDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string FlacStreamDecoder::source_path() const {
    return path_;
}

bool FlacStreamDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_ || decoder_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_.clear();
        block_offset_ = 0;
        queued_samples_ = 0;
        reached_eof_ = false;
    }
    return FLAC__stream_decoder_seek_absolute(decoder_, sample_index) != 0;
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

::FLAC__StreamDecoderWriteStatus FlacStreamDecoder::write_callback(const ::FLAC__StreamDecoder*,
                                                                   const ::FLAC__Frame* frame,
                                                                   const ::FLAC__int32* const buffer[],
                                                                   void* client_data) {
    auto* self = static_cast<FlacStreamDecoder*>(client_data);
    self->handle_write(frame, buffer);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacStreamDecoder::metadata_callback(const ::FLAC__StreamDecoder*,
                                          const ::FLAC__StreamMetadata* metadata,
                                          void* client_data) {
    auto* self = static_cast<FlacStreamDecoder*>(client_data);
    self->handle_metadata(metadata);
}

void FlacStreamDecoder::error_callback(const ::FLAC__StreamDecoder*,
                                       ::FLAC__StreamDecoderErrorStatus status,
                                       void* client_data) {
    auto* self = static_cast<FlacStreamDecoder*>(client_data);
    self->handle_error(status);
}

void FlacStreamDecoder::handle_write(const ::FLAC__Frame* frame, const ::FLAC__int32* const buffer[]) {
    const unsigned blocksize = frame->header.blocksize;
    const unsigned channels = format_.channels;
    PcmBuffer block;
    block.resize(static_cast<std::size_t>(blocksize) * channels);
    if (block.empty()) {
        return;
    }
    std::size_t out = 0;
    for (unsigned i = 0; i < blocksize; ++i) {
        for (unsigned ch = 0; ch < channels; ++ch) {
            block[out++] = static_cast<PcmSample>(buffer[ch][i]);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    queued_samples_ += block.size();
    blocks_.push_back(std::move(block));
}

void FlacStreamDecoder::handle_metadata(const ::FLAC__StreamMetadata* metadata) {
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }
    format_.sample_rate = metadata->data.stream_info.sample_rate;
    format_.channels = metadata->data.stream_info.channels;
    format_.bits_per_sample = static_cast<std::uint16_t>(metadata->data.stream_info.bits_per_sample);
    total_samples_per_channel_ = metadata->data.stream_info.total_samples;
    metadata_seen_ = true;
}

void FlacStreamDecoder::handle_error(::FLAC__StreamDecoderErrorStatus status) {
    const std::string message = FLAC__StreamDecoderErrorStatusString[status];
    Logger::instance().error("FLAC decoder error: " + message);
    std::cerr << "FLAC decoder error: " << message << '\n';
}

void FlacStreamDecoder::fill_queue_if_needed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reached_eof_ || queued_samples_ >= 8192) {
            return;
        }
    }

    Logger::instance().debug("FLAC queue low, decoding next frame");
    if (decoder_ == nullptr || FLAC__stream_decoder_process_single(decoder_) == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_eof_ = true;
        Logger::instance().debug("FLAC process_single returned false, marking EOF");
        return;
    }

    const auto state = FLAC__stream_decoder_get_state(decoder_);
    if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_eof_ = true;
        Logger::instance().debug("FLAC decoder reached end of stream");
    }
}

bool FlacStreamDecoder::threaded_decode_supported() {
    const bool supported = compute_threaded_decode_support();
    g_flac_threaded_decode_supported.store(supported, std::memory_order_relaxed);
    return supported;
}

bool FlacStreamDecoder::threaded_decode_requested() {
    return g_flac_threaded_decode_requested.load(std::memory_order_relaxed);
}

bool FlacStreamDecoder::threaded_decode_active() {
    return g_flac_threaded_decode_active.load(std::memory_order_relaxed);
}

bool FlacStreamDecoder::threaded_decode_enabled() {
    return threaded_decode_requested();
}

void FlacStreamDecoder::set_threaded_decode_enabled(bool enabled) {
    g_flac_threaded_decode_requested.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        g_flac_threaded_decode_active.store(false, std::memory_order_relaxed);
        set_threaded_runtime_status(make_threaded_status_text(false, false, 0));
    }
}

unsigned FlacStreamDecoder::threaded_decode_threads() {
    return threaded_decode_supported() ? flac_thread_count() : 1u;
}

std::string FlacStreamDecoder::threaded_decode_runtime_status() {
    return get_threaded_runtime_status();
}

} // namespace pcmtp
