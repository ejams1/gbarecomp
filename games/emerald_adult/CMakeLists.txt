cmake_minimum_required(VERSION 3.20)
project(emerald_adult_host LANGUAGES C CXX)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the workspace gbarecomp checkout")
if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "gbarecomp source not found at '${GBARECOMP_ROOT}'. Set "
        "-DGBARECOMP_ROOT=<path-to-gbarecomp>.")
endif()

# M0-3 intentionally builds only the host shell. No ROM, BIOS, generated
# cartridge code, or CLI-generated project directory is needed yet.
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

add_executable(EmeraldAdultHost
    src/empty_dispatch_table.cpp
    src/main.cpp)
target_compile_features(EmeraldAdultHost PRIVATE cxx_std_20)
target_link_libraries(EmeraldAdultHost PRIVATE gbarecomp_runtime)
target_include_directories(EmeraldAdultHost PRIVATE "${GBARECOMP_ROOT}/src")
