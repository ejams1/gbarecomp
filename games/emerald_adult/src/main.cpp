#include "runtime/host_window.h"

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--window-smoke";
    if (argc > 2 || (argc == 2 && !smoke &&
                     std::string_view(argv[1]) != "--help")) {
        std::cerr << "Usage: EmeraldAdultHost [--window-smoke|--help]\n";
        return 2;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Emerald Adult host scaffold (cartridge execution is not "
                     "implemented yet).\n"
                     "Usage: EmeraldAdultHost [--window-smoke|--help]\n";
        return 0;
    }

    if (!gbarecomp::HostWindow::is_available()) {
        std::cerr << "This executable has no SDL2 window backend. Reconfigure "
                     "with SDL2 2.30.11 available to gbarecomp.\n";
        return 1;
    }

    gbarecomp::HostWindow window;
    if (!window.open(3, 240, 160, "Pokemon Modern Emerald")) {
        std::cerr << "Could not open the host window. Check the SDL2 video "
                     "driver and desktop session.\n";
        return 1;
    }

    if (!smoke) {
        std::cout << "Host window is open. Cartridge execution is not "
                     "implemented yet; close the window to exit.\n";
        while (!window.pump().quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    window.close();
    return 0;
}
