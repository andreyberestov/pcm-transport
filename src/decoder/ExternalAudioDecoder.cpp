#include "pcmtp/decoder/ExternalAudioDecoder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace pcmtp {
namespace {

std::string run_command_capture(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);
    return output;
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

} // namespace

ExternalAudioDecoder::ExternalAudioDecoder(std::uint32_t forced_output_sample_rate, std::uint16_t forced_output_bits_per_sample, const std::string& resample_quality, const std::string& bitdepth_quality)
    : forced_output_sample_rate_(forced_output_sample_rate),
      forced_output_bits_per_sample_(forced_output_bits_per_sample),
      resample_quality_(resample_quality),
      bitdepth_quality_(bitdepth_quality) {}

ExternalAudioDecoder::~ExternalAudioDecoder() {
    if (pipe_ != nullptr) {
        pclose(pipe_);
        pipe_ = nullptr;
    }
}

std::string ExternalAudioDecoder::to_lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool ExternalAudioDecoder::looks_supported(const std::string& path) {
    static const std::array<const char*, 6> exts = {{".mp3", ".m4a", ".wav", ".ape", ".wv", ".flac"}};
    const std::string ext = to_lower_extension(path);
    for (const char* item : exts) {
        if (ext == item) {
            return true;
        }
    }
    return false;
}

std::string ExternalAudioDecoder::shell_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back(static_cast<char>(39));
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == static_cast<char>(39)) {
            out += "'\''";
        } else {
            out.push_back(value[i]);
        }
    }
    out.push_back(static_cast<char>(39));
    return out;
}

std::string ExternalAudioDecoder::trim_copy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::size_t ExternalAudioDecoder::bytes_per_sample() const {
    if (format_.bits_per_sample <= 16) return 2;
    if (format_.bits_per_sample <= 24) return 3;
    return 4;
}

std::string ExternalAudioDecoder::decode_command(double seconds) const {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(9) << seconds;
    const bool have_seek = seconds > 0.0;
    const std::uint32_t out_rate = forced_output_sample_rate_ > 0 ? forced_output_sample_rate_ : format_.sample_rate;
    const std::uint16_t out_bits = forced_output_bits_per_sample_ > 0 ? forced_output_bits_per_sample_ : format_.bits_per_sample;
    const std::string codec = out_bits <= 16 ? "pcm_s16le"
                             : (out_bits <= 24 ? "pcm_s24le" : "pcm_s32le");
    const std::string raw = out_bits <= 16 ? "s16le"
                           : (out_bits <= 24 ? "s24le" : "s32le");
    const bool need_filter = (out_rate != format_.sample_rate) || (out_bits != format_.bits_per_sample);
    std::string cmd = "ffmpeg -v error -nostdin ";
    if (have_seek) {
        cmd += "-ss " + ss.str() + " ";
    }
    cmd += "-i " + shell_escape(path_) + " ";
    if (need_filter) {
        int precision = 33;
        if (resample_quality_ == "high") precision = 28;
        else if (resample_quality_ == "balanced") precision = 20;
        else if (resample_quality_ == "fast") precision = 16;
        std::string af = "aresample=resampler=soxr:precision=" + std::to_string(precision) + ":cheby=0:osr=" + std::to_string(out_rate);
        if (out_bits <= 16) {
            std::string method = "triangular_hp";
            if (bitdepth_quality_ == "tpdf") method = "triangular";
            else if (bitdepth_quality_ == "rectangular") method = "rectangular";
            af += ":osf=s16:dither_method=" + method;
        } else if (out_bits == 24) {
            af += ":osf=s32";
        } else if (out_bits >= 32) {
            af += ":osf=s32";
        }
        cmd += "-af " + shell_escape(af) + " ";
    } else {
        cmd += "-ar " + std::to_string(out_rate) + " ";
    }
    cmd += "-f " + raw + " -acodec " + codec +
           " -ac " + std::to_string(format_.channels) + " - 2>/dev/null";
    return cmd;
}

GenericTags ExternalAudioDecoder::read_tags(const std::string& path) {
    GenericTags tags;
    const std::string cmd =
        "ffprobe -v error -show_entries format_tags=title,artist,track -of default=nokey=0:noprint_wrappers=1 " +
        shell_escape(path) + " 2>/dev/null";
    const std::string output = run_command_capture(cmd);
    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim_copy(line);
        if (starts_with(line, "TAG:title=") || starts_with(line, "title=")) {
            const std::size_t pos = line.find('=');
            tags.title = line.substr(pos + 1);
        } else if (starts_with(line, "TAG:artist=") || starts_with(line, "artist=")) {
            const std::size_t pos = line.find('=');
            tags.artist = line.substr(pos + 1);
        } else if (starts_with(line, "TAG:track=") || starts_with(line, "track=")) {
            const std::size_t pos = line.find('=');
            try {
                tags.track_number = std::stoi(line.substr(pos + 1));
            } catch (...) {
            }
        }
    }
    return tags;
}

void ExternalAudioDecoder::open(const std::string& path) {
    if (!looks_supported(path)) {
        throw std::runtime_error("ExternalAudioDecoder does not support this file type");
    }
    if (pipe_ != nullptr) {
        pclose(pipe_);
        pipe_ = nullptr;
    }

    path_ = path;
    format_.sample_rate = 44100;
    format_.channels = 2;
    format_.bits_per_sample = 16;
    reached_eof_ = false;
    total_samples_per_channel_ = 0;
    current_samples_per_channel_ = 0;

    const std::string probe_cmd =
        "ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate,channels,bits_per_sample,bits_per_raw_sample,duration:format=duration -of default=nokey=0:noprint_wrappers=1 " +
        shell_escape(path) + " 2>/dev/null";
    const std::string probe_out = run_command_capture(probe_cmd);
    std::istringstream ps(probe_out);
    std::string line;
    double seconds = 0.0;
    while (std::getline(ps, line)) {
        line = trim_copy(line);
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        try {
            if (key == "sample_rate") format_.sample_rate = static_cast<std::uint32_t>(std::stoul(value));
            else if (key == "channels") format_.channels = static_cast<std::uint16_t>(std::stoul(value));
            else if ((key == "bits_per_sample" || key == "bits_per_raw_sample") && !value.empty() && value != "N/A") {
                format_.bits_per_sample = static_cast<std::uint16_t>(std::stoul(value));
            } else if (key == "duration") {
                const double probed = std::stod(value);
                if (probed > seconds) {
                    seconds = probed;
                }
            }
        } catch (...) {}
    }
    if (format_.channels == 0) format_.channels = 2;
    if (format_.bits_per_sample != 16 && format_.bits_per_sample != 24 && format_.bits_per_sample != 32) {
        format_.bits_per_sample = 16;
    }
    if (forced_output_sample_rate_ > 0) {
        format_.sample_rate = forced_output_sample_rate_;
    }
    if (forced_output_bits_per_sample_ == 16 || forced_output_bits_per_sample_ == 24 || forced_output_bits_per_sample_ == 32) {
        format_.bits_per_sample = forced_output_bits_per_sample_;
    }
    if (seconds > 0.0 && format_.sample_rate > 0) {
        total_samples_per_channel_ = static_cast<std::uint64_t>(std::llround(seconds * static_cast<double>(format_.sample_rate)));
    }

    const std::string decode_cmd = decode_command(0.0);
    pipe_ = popen(decode_cmd.c_str(), "r");
    if (pipe_ == nullptr) {
        throw std::runtime_error("Cannot start ffmpeg decoder");
    }
    opened_ = true;
}

const AudioFormat& ExternalAudioDecoder::format() const {
    return format_;
}

std::size_t ExternalAudioDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_ || pipe_ == nullptr) {
        throw std::runtime_error("Decoder not opened");
    }

    const std::size_t bps = bytes_per_sample();
    std::vector<unsigned char> raw(max_samples * bps);
    const std::size_t got_bytes = fread(raw.data(), 1, raw.size(), pipe_);
    const std::size_t got = got_bytes / bps;
    for (std::size_t i = 0; i < got; ++i) {
        const unsigned char* src = raw.data() + i * bps;
        std::int32_t value = 0;
        if (bps == 2) {
            value = static_cast<std::int16_t>(static_cast<std::uint16_t>(src[0]) | (static_cast<std::uint16_t>(src[1]) << 8));
        } else if (bps == 3) {
            std::uint32_t u = static_cast<std::uint32_t>(src[0]) |
                              (static_cast<std::uint32_t>(src[1]) << 8) |
                              (static_cast<std::uint32_t>(src[2]) << 16);
            if ((u & 0x00800000u) != 0) {
                u |= 0xFF000000u;
            }
            value = static_cast<std::int32_t>(u);
        } else {
            std::uint32_t u = static_cast<std::uint32_t>(src[0]) |
                              (static_cast<std::uint32_t>(src[1]) << 8) |
                              (static_cast<std::uint32_t>(src[2]) << 16) |
                              (static_cast<std::uint32_t>(src[3]) << 24);
            value = static_cast<std::int32_t>(u);
        }
        destination[i] = value;
    }
    current_samples_per_channel_ += got / std::max<std::uint16_t>(1, format_.channels);
    if (got < max_samples && feof(pipe_) != 0) {
        reached_eof_ = true;
    }
    return got;
}

bool ExternalAudioDecoder::eof() const {
    return reached_eof_;
}

std::uint64_t ExternalAudioDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string ExternalAudioDecoder::source_path() const {
    return path_;
}

bool ExternalAudioDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_) return false;
    const double seconds = format_.sample_rate > 0
        ? static_cast<double>(sample_index) / static_cast<double>(format_.sample_rate)
        : 0.0;
    if (pipe_ != nullptr) {
        pclose(pipe_);
        pipe_ = nullptr;
    }
    reached_eof_ = false;
    current_samples_per_channel_ = sample_index;
    const std::string decode_cmd = decode_command(seconds);
    pipe_ = popen(decode_cmd.c_str(), "r");
    return pipe_ != nullptr;
}

} // namespace pcmtp
