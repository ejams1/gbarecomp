# Pokemon Modern Emerald host scaffold

This is a window-lifecycle stub for the optional native host. It opens an SDL2
window and exits when the window is closed. `--window-smoke` opens and closes
the window immediately to check initialization and cleanup. **Cartridge
execution is not implemented until M0-4.** This target does not compile or link
generated game code.

## Pinned inputs

- Framework: workspace `gbarecomp` commit `8bab64e` (M0-2 integration study).
- SDL: 2.30.11, upstream commit
  `fa24d868ac2f8fd558e4e914c9863411245db8fd`.
- CMake: 3.20 or newer; C++20 compiler and Ninja are required by the framework.
- Docker Linux toolchain: Debian bookworm base digest
  `7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8`, Debian
  snapshot `20261024T000000Z`, CMake 3.25.1, Clang 14.0.6, Ninja 1.11.1.

On Windows, install SDL 2.30.11 into the MSYS2 MinGW64 prefix expected by the
framework (`C:/msys64/mingw64`), or set `GBARECOMP_MINGW_PREFIX_UNIX` to its
MSYS2 Unix path. The framework toolchain file
`../../cmake/toolchains/clang-mingw.cmake` expects LLVM and MSYS2 MinGW64; set
`GBARECOMP_LLVM_ROOT` and `GBARECOMP_MINGW_ROOT` if they are elsewhere. CMake
stops with a specific message when SDL is missing or its version differs.
To build the pinned SDL release into the default prefix from PowerShell:

```powershell
$sdlSource = Join-Path $env:TEMP "SDL-2.30.11"
git clone https://github.com/libsdl-org/SDL.git $sdlSource
git -C $sdlSource checkout --detach fa24d868ac2f8fd558e4e914c9863411245db8fd
cmake -S $sdlSource -B (Join-Path $sdlSource "build") -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_INSTALL_PREFIX=C:/msys64/mingw64 `
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF
cmake --build (Join-Path $sdlSource "build") --parallel
cmake --install (Join-Path $sdlSource "build")
```

The framework's `external/arm-recomp-core` submodule must also be populated at
the exact gitlink recorded by the checked-out framework commit.

## Configure and build

Run these commands from `gbarecomp/games/emerald_adult` in PowerShell:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchains/clang-mingw.cmake"
cmake --build build --target EmeraldAdultHost --parallel
```

Open the stub window with `build/EmeraldAdultHost.exe`; run
`build/EmeraldAdultHost.exe --window-smoke` for a short open/close lifecycle
check. A graphical desktop session is needed for the visible-window check.
The window-only target links `src/empty_dispatch_table.cpp` because the shared
runtime expects dispatch symbols even when no cartridge is linked. Its zero
entry count deliberately makes game execution impossible; M0-4 will switch
the executable to generated cartridge dispatch output.

## Docker build and headless lifecycle check

The pinned Linux image supplies CMake, Ninja, a C++ compiler, and SDL2 2.30.11
with dummy video/audio drivers. From the `gbarecomp` root:

```powershell
docker build -f games/emerald_adult/Dockerfile -t emerald-adult-host-build .
docker run --rm -v "${PWD}:/workspace" -w /workspace/games/emerald_adult `
  emerald-adult-host-build sh -lc `
  'cmake -S . -B build-docker-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-docker-clang --target EmeraldAdultHost --parallel && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build-docker-clang/EmeraldAdultHost --window-smoke'
```

The Docker check validates SDL window creation and teardown without a desktop;
it does not replace checking the visible Windows window with the Windows
toolchain above.

## Local user files

When cartridge execution is added, keep user-supplied inputs and saves under
`local/` beside this README:

- `local/pokeemerald_modern.gba` — built ROM copied from the game repository.
- `local/gba_bios.bin` — user-provided BIOS dump.
- `local/saves/` — disposable and player cartridge saves.

These paths are ignored by Git. The ROM and BIOS are not distributed with this
project. The current stub does not read them or create save data; M0-4 will add
the runtime command-line/configuration path and identity checks.
