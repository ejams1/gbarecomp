#include "host_window.h"
#include "runtime.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--window-smoke";
    if (smoke) {
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
        window.close();
        return 0;
    }

    gbarecomp::RunOptions options;
    options.builtin_game_name = "Pokemon Modern Emerald";
    options.builtin_rom_sha1 = "57d3561262bf5337293a2d31f6b79b5879f7cff7";
    options.default_game_config_path = GBARECOMP_DEFAULT_GAME_CONFIG;
    options.strict_bios_sha1 = true;
    options.strict_rom_sha1 = true;
    return gbarecomp::run_game(argc, argv, options);
}
