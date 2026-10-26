#include "host_window.h"
#include "runtime.h"
#include "scene_media_observer.h"

#include <iostream>
#include <string_view>

#ifndef GBARECOMP_EMERALD_ROM_SHA1
#error "CMake must derive the built-in ROM identity from emerald.toml"
#endif

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
    options.builtin_rom_sha1 = GBARECOMP_EMERALD_ROM_SHA1;
    options.mod_game_id = "pokemon_modern_emerald";
    options.default_game_config_path = GBARECOMP_DEFAULT_GAME_CONFIG;
    options.strict_bios_sha1 = true;
    options.strict_rom_sha1 = true;
    emerald_media::configure_scene_media(options);
    return gbarecomp::run_game(argc, argv, options);
}
