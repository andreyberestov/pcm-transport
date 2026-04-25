#include <exception>
#include <iostream>

#include "pcmtp/app/Application.hpp"

int main(int argc, char** argv) {
    try {
        pcmtp::Application app;
        return app.run(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
