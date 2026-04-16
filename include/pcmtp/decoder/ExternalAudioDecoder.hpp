#pragma once

#include <cstdio>
#include <string>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

struct GenericTags {
    std::string title;
    std::string artist;
    int track_number = 0;
};

class ExternalAudioDecoder final : public IAudioDecoder {
public:
    explicit ExternalAudioDecoder(std::uint32_t forced_output_sample_rate = 0, std::uint16_t forced_output_bits_per_sample = 0, const std::string& resample_quality = "maximum", const std::string& bitdepth_quality = "tpdf_hp");
    ~ExternalAudioDecoder() override;

    void open(const std::string& path) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;

    static bool looks_supported(const std::string& path);
    static GenericTags read_tags(const std::string& path);

private:
    static std::string to_lower_extension(const std::string& path);
    static std::string shell_escape(const std::string& value);
    static std::string trim_copy(const std::string& value);
    std::string decode_command(double seconds) const;
    std::size_t bytes_per_sample() const;

    std::uint32_t forced_output_sample_rate_ = 0;
    std::uint16_t forced_output_bits_per_sample_ = 0;
    std::string resample_quality_ = "maximum";
    std::string bitdepth_quality_ = "tpdf_hp";
    FILE* pipe_ = nullptr;
    AudioFormat format_{};
    std::uint64_t total_samples_per_channel_ = 0;
    std::string path_;
    bool opened_ = false;
    bool reached_eof_ = false;
    std::uint64_t current_samples_per_channel_ = 0;
};

} // namespace pcmtp
