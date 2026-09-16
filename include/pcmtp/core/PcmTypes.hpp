// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pcmtp {

struct AudioFormat {
    std::uint32_t sample_rate = 44100;
    std::uint16_t channels = 2;
    std::uint16_t bits_per_sample = 16;

    bool is_red_book() const {
        return sample_rate == 44100 && channels == 2 && bits_per_sample == 16;
    }

    std::string to_string() const {
        return std::to_string(sample_rate) + " Hz / " +
               std::to_string(bits_per_sample) + "-bit / " +
               (channels == 2 ? std::string("stereo") : std::to_string(channels) + " ch");
    }
};

enum class Pcm16QuantizationMode {
    RoundToNearestEven,
    RoundHalfUp,
    Truncate
};

enum class Pcm16QuantizationRuntimeKind {
    NotUsed,
    RoundToNearestEven,
    RoundHalfUp,
    Truncate,
    Ffmpeg
};

enum class Pcm16DitherMode {
    Off,
    HighPassClassic,
    HighPassMild
};

enum class Pcm16DitherRuntimeKind {
    NotUsed,
    HighPassClassic,
    HighPassMild
};

enum class DecoderPcmSampleKind {
    Unknown,
    U8,
    U8Planar,
    S16,
    S16Planar,
    S32,
    S32Planar,
    Float,
    FloatPlanar,
    Double,
    DoublePlanar,
    S64,
    S64Planar
};

// PCM storage and PCM processing semantics are intentionally separate from
// AudioFormat. NativeInt32 carries exact integer PCM code values. Float32 and
// Float64 are one normalized floating domain: unity is 1.0 and over-range
// finite samples are preserved until an explicit output boundary.
enum class PcmStorageFormat {
    NativeInt32,
    Float32,
    Float64
};

enum class PcmOutputFormat {
    S16,
    S24,
    S32,
    Float32,
    Float64
};

inline const char* pcm_storage_format_name(PcmStorageFormat format) noexcept {
    switch (format) {
    case PcmStorageFormat::NativeInt32: return "NativeInt32";
    case PcmStorageFormat::Float32: return "Float32";
    case PcmStorageFormat::Float64: return "Float64";
    }
    return "unknown";
}

inline const char* pcm_output_format_name(PcmOutputFormat format) noexcept {
    switch (format) {
    case PcmOutputFormat::S16: return "16-bit";
    case PcmOutputFormat::S24: return "24-bit";
    case PcmOutputFormat::S32: return "32-bit";
    case PcmOutputFormat::Float32: return "Float32";
    case PcmOutputFormat::Float64: return "Float64";
    }
    return "unknown";
}

inline std::uint16_t pcm_output_integer_bits(PcmOutputFormat format) noexcept {
    switch (format) {
    case PcmOutputFormat::S16: return 16;
    case PcmOutputFormat::S24: return 24;
    case PcmOutputFormat::S32: return 32;
    case PcmOutputFormat::Float32: return 0;
    case PcmOutputFormat::Float64: return 0;
    }
    return 0;
}

inline bool pcm_output_is_floating(PcmOutputFormat format) noexcept {
    return format == PcmOutputFormat::Float32 ||
           format == PcmOutputFormat::Float64;
}

using PcmSample = std::int32_t;
using PcmBuffer = std::vector<PcmSample>;
using Float32PcmBuffer = std::vector<float>;
using Float64PcmBuffer = std::vector<double>;

class PcmBlock final {
public:
    using Storage = std::variant<PcmBuffer, Float32PcmBuffer, Float64PcmBuffer>;

    PcmBlock() = default;
    explicit PcmBlock(PcmStorageFormat format) { ensure_storage(format); }

    PcmStorageFormat storage_format() const noexcept {
        switch (storage_.index()) {
        case 1: return PcmStorageFormat::Float32;
        case 2: return PcmStorageFormat::Float64;
        case 0:
        default:
            return PcmStorageFormat::NativeInt32;
        }
    }

    std::size_t size() const noexcept {
        return std::visit([](const auto& buffer) { return buffer.size(); }, storage_);
    }

    bool empty() const noexcept { return size() == 0; }

    void clear() noexcept {
        std::visit([](auto& buffer) { buffer.clear(); }, storage_);
    }

    void ensure_storage(PcmStorageFormat format) {
        if (storage_format() == format) {
            return;
        }
        if (!empty()) {
            throw std::logic_error("Cannot change PCM block storage with live samples");
        }
        switch (format) {
        case PcmStorageFormat::NativeInt32:
            storage_.emplace<PcmBuffer>();
            break;
        case PcmStorageFormat::Float32:
            storage_.emplace<Float32PcmBuffer>();
            break;
        case PcmStorageFormat::Float64:
            storage_.emplace<Float64PcmBuffer>();
            break;
        }
        if (reserve_hint_ > 0) {
            std::visit([this](auto& buffer) { buffer.reserve(reserve_hint_); }, storage_);
        }
    }

    void resize(std::size_t sample_count) {
        std::visit([sample_count](auto& buffer) { buffer.resize(sample_count); }, storage_);
    }

    void reserve(std::size_t sample_count) {
        reserve_hint_ = sample_count;
        std::visit([sample_count](auto& buffer) { buffer.reserve(sample_count); }, storage_);
    }

    PcmBuffer& native() { return std::get<PcmBuffer>(storage_); }
    const PcmBuffer& native() const { return std::get<PcmBuffer>(storage_); }
    Float32PcmBuffer& float32() { return std::get<Float32PcmBuffer>(storage_); }
    const Float32PcmBuffer& float32() const { return std::get<Float32PcmBuffer>(storage_); }
    Float64PcmBuffer& float64() { return std::get<Float64PcmBuffer>(storage_); }
    const Float64PcmBuffer& float64() const { return std::get<Float64PcmBuffer>(storage_); }

    void append(const PcmSample* samples, std::size_t count) {
        ensure_storage(PcmStorageFormat::NativeInt32);
        if (count == 0) return;
        if (samples == nullptr) {
            throw std::invalid_argument("PCM block append received null samples");
        }
        native().insert(native().end(), samples, samples + count);
    }

    void append(const float* samples, std::size_t count) {
        ensure_storage(PcmStorageFormat::Float32);
        if (count == 0) return;
        if (samples == nullptr) {
            throw std::invalid_argument("PCM block append received null samples");
        }
        float32().insert(float32().end(), samples, samples + count);
    }

    void append(const double* samples, std::size_t count) {
        ensure_storage(PcmStorageFormat::Float64);
        if (count == 0) return;
        if (samples == nullptr) {
            throw std::invalid_argument("PCM block append received null samples");
        }
        float64().insert(float64().end(), samples, samples + count);
    }

    void append_from(const PcmBlock& source,
                     std::size_t offset,
                     std::size_t count) {
        if (offset > source.size() || count > source.size() - offset) {
            throw std::out_of_range("PCM block append range is invalid");
        }
        if (count == 0) {
            return;
        }
        ensure_storage(source.storage_format());
        switch (source.storage_format()) {
        case PcmStorageFormat::NativeInt32:
            append(source.native().data() + offset, count);
            break;
        case PcmStorageFormat::Float32:
            append(source.float32().data() + offset, count);
            break;
        case PcmStorageFormat::Float64:
            append(source.float64().data() + offset, count);
            break;
        }
    }

    void erase_range(std::size_t first, std::size_t last) {
        if (first > last || last > size()) {
            throw std::out_of_range("PCM block erase range is invalid");
        }
        std::visit([first, last](auto& buffer) {
            buffer.erase(buffer.begin() + static_cast<std::ptrdiff_t>(first),
                         buffer.begin() + static_cast<std::ptrdiff_t>(last));
        }, storage_);
    }

private:
    Storage storage_{PcmBuffer{}};
    std::size_t reserve_hint_ = 0;
};

inline std::int64_t pcm_full_scale(std::uint16_t bits_per_sample) {
    if (bits_per_sample >= 32) return 2147483647LL;
    if (bits_per_sample <= 1) return 1LL;
    return (1LL << (bits_per_sample - 1)) - 1LL;
}

} // namespace pcmtp
