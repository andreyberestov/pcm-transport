#pragma once

#include <string>

namespace pcmtp {

class Application {
public:
    int run(int argc, char** argv);

private:
    int run_probe_only();
    int run_player(const std::string& file_path, const std::string& device_name, std::size_t transport_buffer_ms);
    int run_gui(int argc, char** argv, std::size_t transport_buffer_ms);
    void print_usage(const char* program_name) const;
};

} // namespace pcmtp
