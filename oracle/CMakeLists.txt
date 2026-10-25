# oracle/CMakeLists.txt — mGBA-backed reference emulator.
#
# This subtree is NEVER pulled into the main build by default; gate is
# `GBARECOMP_BUILD_ORACLE`. The oracle binary embeds libmgba (MPL-2.0)
# and stays a separate process — see ../PRINCIPLES.md "zero copyleft
# emulator dependencies in the native build".

cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    project(gbarecomp_oracle LANGUAGES C CXX)
endif()

set(MGBA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/mgba" CACHE PATH
    "Path to the mGBA source checkout used by the external oracle")
set(MGBA_BUILD ${MGBA_ROOT}/build)
set(MGBA_LIB   ${MGBA_BUILD}/libmgba.a)

if(NOT EXISTS ${MGBA_LIB})
    message(FATAL_ERROR
        "GBARECOMP_BUILD_ORACLE=ON but libmgba.a missing at ${MGBA_LIB}. "
        "Configure + build third_party/mgba/ first.")
endif()

# MSYS GCC emits Windows absolute include paths (`C:/...`) in depfiles.
# In this subdirectory Make then sees them as `oracle/C:/...`, which
# contains multiple target separators. Let CMake scan oracle deps.
set(CMAKE_DEPENDS_USE_COMPILER FALSE)

add_executable(gbarecomp_oracle main.cpp)
target_include_directories(gbarecomp_oracle PRIVATE
    ${MGBA_ROOT}/include
    ${MGBA_BUILD}/include
)
target_link_libraries(gbarecomp_oracle PRIVATE
    ${MGBA_LIB}
)
if(UNIX)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(PNG REQUIRED)
    target_link_libraries(gbarecomp_oracle PRIVATE
        Threads::Threads ZLIB::ZLIB PNG::PNG ${CMAKE_DL_LIBS} m)
endif()
if(WIN32 OR MSYS)
    # mGBA's config.c pulls SHGetKnownFolderPath + CoTaskMemFree for
    # locating %APPDATA%, so we need ole32 + uuid on top of the
    # basic socket libs.
    target_link_libraries(gbarecomp_oracle PRIVATE
        ws2_32 shlwapi winmm ole32 uuid)
elseif(APPLE)
    # Same function, other platform: mCoreConfigPortablePath uses
    # CFBundleGetMainBundle / CFURL* / kCFAllocatorDefault on macOS, so
    # libmgba needs CoreFoundation. Without it the oracle fails to link
    # with undefined _CFBundle*/_CFURL*/_kCFAllocatorDefault.
    target_link_libraries(gbarecomp_oracle PRIVATE
        "-framework CoreFoundation")
endif()
target_compile_definitions(gbarecomp_oracle PRIVATE
    M_CORE_GBA=1
    MGBA_ORACLE_VERSION=\"0.10.5\"
    MGBA_ORACLE_COMMIT=\"26b7884bc25a5933960f3cdcd98bac1ae14d42e2\"
)
