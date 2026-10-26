cmake_minimum_required(VERSION 3.20)
project(emerald_adult_host LANGUAGES C CXX)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the workspace gbarecomp checkout")
if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "gbarecomp source not found at '${GBARECOMP_ROOT}'. Set "
        "-DGBARECOMP_ROOT=<path-to-gbarecomp>.")
endif()

set(GBARECOMP_GENERATED_BIOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_bios"
    CACHE PATH "Directory containing this game's generated BIOS sources")
set(GBARECOMP_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    CACHE PATH "Directory containing this game's generated cartridge sources")
set(_bios_cpp "${GBARECOMP_GENERATED_BIOS_DIR}/bios_recompiled.cpp")
set(_bios_dispatch "${GBARECOMP_GENERATED_BIOS_DIR}/bios_dispatch_table.cpp")
if(NOT EXISTS "${_bios_cpp}" OR NOT EXISTS "${_bios_dispatch}")
    message(FATAL_ERROR
        "Generated BIOS sources are missing from '${GBARECOMP_GENERATED_BIOS_DIR}'. "
        "Run gba_recompile --bios with bios/gba_bios.toml before configuring the host.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/emerald.toml")
    message(FATAL_ERROR
        "Hand-maintained emerald.toml is missing; it pins the ROM, BIOS, save "
        "configuration, entry root, and identity.")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/emerald.toml" _emerald_config_text)
string(FIND "${_emerald_config_text}" "[identity]" _identity_start)
string(FIND "${_emerald_config_text}" "[rom]" _rom_start)
string(FIND "${_emerald_config_text}" "[bios]" _bios_start)
if(_identity_start LESS 0 OR _rom_start LESS _identity_start OR _bios_start LESS _rom_start)
    message(FATAL_ERROR "emerald.toml must define [identity], [rom], and [bios] sections in order.")
endif()
math(EXPR _identity_length "${_rom_start} - ${_identity_start}")
math(EXPR _rom_length "${_bios_start} - ${_rom_start}")
string(SUBSTRING "${_emerald_config_text}" ${_identity_start} ${_identity_length} _identity_section)
string(SUBSTRING "${_emerald_config_text}" ${_rom_start} ${_rom_length} _rom_section)
string(REGEX MATCH "sha1[ \t]*=[ \t]*\"([0-9A-Fa-f]+)\"" _identity_sha1_match "${_identity_section}")
set(_emerald_identity_sha1 "${CMAKE_MATCH_1}")
string(REGEX MATCH "sha1[ \t]*=[ \t]*\"([0-9A-Fa-f]+)\"" _rom_sha1_match "${_rom_section}")
set(_emerald_rom_sha1 "${CMAKE_MATCH_1}")
string(LENGTH "${_emerald_identity_sha1}" _identity_sha1_length)
string(LENGTH "${_emerald_rom_sha1}" _rom_sha1_length)
if(NOT _identity_sha1_length EQUAL 40 OR NOT _rom_sha1_length EQUAL 40 OR
   NOT _emerald_identity_sha1 MATCHES "^[0-9A-Fa-f]+$" OR
   NOT _emerald_rom_sha1 MATCHES "^[0-9A-Fa-f]+$" OR
   NOT _emerald_identity_sha1 STREQUAL _emerald_rom_sha1)
    message(FATAL_ERROR
        "emerald.toml [identity].sha1 and [rom].sha1 must contain the same 40-digit ROM SHA-1.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/host_scene_state_address.h")
    message(FATAL_ERROR
        "Imported scene-state address is missing. Run build-exe.ps1 so the matching ELF data-symbol table can generate it.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/symbols/POKEMON_MODERN_EMERALD_symbols.toml")
    message(FATAL_ERROR
        "Imported fork symbols are missing. Run tools/import-symbols.ps1 "
        "against the pinned ROM/ELF/map before generation.")
endif()

# The recompiler CLI is bootstrapped separately before this game project is
# configured. This host build consumes generated BIOS and cartridge files from
# separate, ignored output directories.
# The scene observer uses the existing trusted-plugin activation and immutable
# presentation APIs. Keep framework support enabled even without a package so
# registration stays inert and package absence preserves the ROM view.
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)
add_subdirectory("${GBARECOMP_ROOT}" "${CMAKE_BINARY_DIR}/gbarecomp")

get_target_property(_runtime_compile_definitions gbarecomp_runtime
    INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_runtime_include_directories gbarecomp_runtime
    INTERFACE_INCLUDE_DIRECTORIES)
if(NOT "GBARECOMP_HAVE_SDL2" IN_LIST _runtime_compile_definitions)
    message(FATAL_ERROR
        "Emerald host requires the SDL2 backend, but this gbarecomp build "
        "did not find SDL2. Install the pinned SDL2 2.30.11 development "
        "package (see README.md), then configure again.")
endif()

set(_sdl_version_header "")
foreach(_include_dir IN LISTS _runtime_include_directories)
    if(EXISTS "${_include_dir}/SDL_version.h")
        set(_sdl_version_header "${_include_dir}/SDL_version.h")
        break()
    endif()
endforeach()
if(NOT _sdl_version_header)
    message(FATAL_ERROR
        "gbarecomp enabled SDL2 but did not export SDL_version.h through "
        "gbarecomp_runtime; check the SDL2 development installation.")
endif()

file(STRINGS "${_sdl_version_header}" _sdl_version_lines
    REGEX "^#define SDL_(MAJOR_VERSION|MINOR_VERSION|PATCHLEVEL) +[0-9]+$")
foreach(_part MAJOR_VERSION MINOR_VERSION PATCHLEVEL)
    string(REGEX MATCH "SDL_${_part} +([0-9]+)"
        _match "${_sdl_version_lines}")
    string(REPLACE "_VERSION" "" _short_part "${_part}")
    if(_part STREQUAL "PATCHLEVEL")
        set(_short_part PATCH)
    endif()
    set(_sdl_${_short_part} "${CMAKE_MATCH_1}")
endforeach()
if(NOT "${_sdl_MAJOR}.${_sdl_MINOR}.${_sdl_PATCH}" STREQUAL "2.30.11")
    message(FATAL_ERROR
        "Expected SDL2 2.30.11, found ${_sdl_MAJOR}.${_sdl_MINOR}.${_sdl_PATCH} "
        "at '${_sdl_version_header}'. Install the pinned development package "
        "or correct GBARECOMP_MINGW_PREFIX_UNIX.")
endif()

include("${CMAKE_BINARY_DIR}/gbarecomp/runtime.cmake")
gbarecomp_add_runtime_target(EmeraldAdultHost
    GAME_GENERATED_DIR "${GBARECOMP_GENERATED_DIR}"
    DEBUG_PORT 19842
    WINDOW_TITLE "Pokemon Modern Emerald"
    DEFAULT_GAME_CONFIG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/emerald.toml")
# The framework archive contains runtime_arm.cpp (which references the BIOS
# dispatch table) and generated BIOS translation units. Attach the generated
# BIOS objects directly to the executable so their definitions are available
# before the static runtime archives are scanned by traditional linkers.
target_sources(EmeraldAdultHost PRIVATE
    src/main.cpp
    src/scene_media_observer.cpp
    src/gallery_media_package.cpp
    "${_bios_cpp}"
    "${_bios_dispatch}")
target_compile_definitions(EmeraldAdultHost PRIVATE
    GBARECOMP_EMERALD_ROM_SHA1=\"${_emerald_identity_sha1}\")

# The shared helper currently wires executable shards, the dispatch table,
# and PC symbols. Include the optional data-name table emitted for --data-symbols.
set(_data_symbol_map "${GBARECOMP_GENERATED_DIR}/data_symbol_map.cpp")
if(EXISTS "${_data_symbol_map}")
    target_sources(EmeraldAdultHost PRIVATE "${_data_symbol_map}")
endif()
target_compile_features(EmeraldAdultHost PRIVATE cxx_std_20)

add_executable(gallery_media_package_tests
    tests/gallery_media_package_tests.cpp
    src/gallery_media_package.cpp)
target_include_directories(gallery_media_package_tests PRIVATE
    src "${GBARECOMP_ROOT}/src/runtime")
target_compile_features(gallery_media_package_tests PRIVATE cxx_std_20)
enable_testing()
add_test(NAME gallery_media_package_tests COMMAND gallery_media_package_tests)
