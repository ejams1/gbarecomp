# Pokemon Modern Emerald host scaffold

This host recompiles the verified Modern Emerald ROM and runs it through the
shared gbarecomp runtime. Its `--window-smoke` option opens and closes the SDL2
window without booting the game. The default launch reads the hand-maintained
`emerald.toml`, verifies the ROM/BIOS identities, and uses the configured
Flash1M save. RTC support is auto-detected from the cartridge's Seiko SDK
signature; set `RECOMP_RTC_EPOCH` only for deterministic development runs.

## Pinned inputs

- Framework generation CLI source: workspace `gbarecomp` commit
  `c96bded93d5a39f0e8707a53e2fae96ac42452f2` (M0-4a); its verified binary
  SHA-256 is `3ba3a74d859f20c21c2c6f91bd213b90350a3d9843c47db28b228734f530e716`.
  The host runtime includes M0-4's ABI 7 overlay callback fix.
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

## Reproducible pipeline

The one-command pipeline resolves the default sibling game repository from
this file and passes paths containing spaces as native arguments. From the
workspace root, launch it with:

```powershell
& "./gbarecomp/games/emerald_adult/build-exe.ps1"
```

You can also invoke the script by absolute path from another working
directory. Install the pinned ROM image using the game repository's
`docs/build-notes.md` instructions. The local machine needs PowerShell, Docker,
and Python. The host image supplies the pinned Debian,
CMake, Clang, Ninja and SDL2 toolchain. Copy the user-provided BIOS to
`gbarecomp/bios/gba_bios.bin`; BIOS files are not distributed.

For the first build, or after intentionally changing the ROM, ELF/map, framework
revision, compiler image, or importer output, explicitly regenerate from the
ROM produced by the pinned build:

```powershell
& "./gbarecomp/games/emerald_adult/build-exe.ps1" -RegenerateIdentity
```

The script runs `make modern` in `adult-emerald-build:r63-libpng`, verifies the
identity against `emerald.toml`, captures ELF sections/symbols, imports symbol
and data-symbol tables, builds the recompiler CLI, generates BIOS and cartridge
translation, then configures and builds `EmeraldAdultHost`. Regeneration updates
the hand-maintained ROM SHA-1 from the built ROM and the generated import
overlay from the matching ELF/map. It records the Docker image ID, framework
commit, recompiler version/hash, input hashes, generator fingerprint, and
executable hash under ignored `symbols/build-manifest.json`. The headless Linux
executable and one-frame startup smoke capture are under ignored
`build-pipeline/`. The existing M0-5 title-menu route remains the longer
playability check.
Use the Windows toolchain below if you also need a native Windows executable
with a visible window.

Normal rebuilds use the same command without the switch:

```powershell
& "./gbarecomp/games/emerald_adult/build-exe.ps1"
```

If the ROM or derived import metadata has drifted, a normal build prints old
and new hashes and stops. It prints the exact `-RegenerateIdentity` command to
review and run. Generated cartridge/BIOS sources and the host CMake cache are
removed only after resolved paths are checked to stay in this host's three
generated-output directories. `local/saves/` is never part of cleanup.

## Manual configure and build

First build the recompiler CLI from the `gbarecomp` root:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target gba_recompile --parallel
```

Build `pokeemerald_modern.gba`, `.elf`, and `.map` together with `make modern`
in the pinned game Docker image. Then from the workspace root run:

```powershell
./gbarecomp/games/emerald_adult/tools/import-symbols.ps1
```

Generate the BIOS and cartridge code with the CLI from `gbarecomp`:

```powershell
$engineRoot = (Resolve-Path ./gbarecomp).Path
$hostRoot = Join-Path $engineRoot "games/emerald_adult"
$recompiler = Join-Path $engineRoot "build/gba_recompile.exe"
& $recompiler --bios (Join-Path $engineRoot "bios/gba_bios.bin") `
  --config (Join-Path $engineRoot "bios/gba_bios.toml") `
  --out (Join-Path $hostRoot "generated_bios")
& $recompiler --rom (Join-Path (Resolve-Path ./pokeemerald-modern-adult) "pokeemerald_modern.gba") `
  --symbols (Join-Path $hostRoot "symbols/imported_symbols.tsv") `
  --data-symbols (Join-Path $hostRoot "symbols/imported_data_symbols.tsv") `
  --config (Join-Path $hostRoot "emerald.toml") `
  --config (Join-Path $hostRoot "symbols/POKEMON_MODERN_EMERALD_symbols.toml") `
  --out (Join-Path $hostRoot "generated")
```

Then configure and build the host from `gbarecomp/games/emerald_adult`:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchains/clang-mingw.cmake" `
  "-DGBARECOMP_GENERATED_BIOS_DIR=${PWD}/generated_bios"
cmake --build build --target EmeraldAdultHost --parallel
```

The host fails configuration when BIOS, cartridge shards, or imported identity
metadata are missing. This game runner enforces the configured BIOS and ROM
SHA-1 values; missing files and hash mismatches stop startup with a specific
diagnostic. The generated directories stay ignored. CMake links sharded
cartridge code, BIOS translation and the data symbol map when present.

### Optional gallery video package

The native host accepts media packages built from the game repository's
`tools/gallery/build_host_media.py` outputs. That pipeline supports source
videos at 240x160, 160x90, and 90x160; it converts each to 240x160 BGR555LE
frames with black bars where needed and creates mono S16 PCM matched to the
guest VBlank timeline. The GBA fallback continues to play without host media.

After generating an M5-5 output directory, create a data-only package and give
it the license that applies to those derived outputs:

```powershell
python ./games/emerald_adult/tools/package_gallery_media.py `
  ./pokeemerald-modern-adult/build/host-media/real-e2e/packages/neutral_coast `
  ./pokeemerald-modern-adult/build/host-media/adult.gallery-media.gbamod `
  --license-id PolyForm-Noncommercial-1.0.0 `
  --license-file ./gbarecomp/LICENSE
```

In the host launcher's Mods page, install the `.gbamod`, leave the feature off
unless desired, then select it and use **Play/Commit**. The package targets the
`pokemon_modern_emerald` game ID and ROM SHA-1 pinned in `emerald.toml`. Its
frame chunks, audio, and provenance manifest are verified when installed and
when the catalog is loaded. To update media, build a new package version with
`--version`, install it through Mods, select it, and commit again. With the
package absent or its feature disabled, ordinary
ROM playback remains available.

### Backend parity trace

The optional host media backend can be checked against canonical guest scene
records captured by the required BIOS mGBA runner. First build the current host
and create the meadow/coast `.gbamod` files as above. Then run
`tools/run_host_media_trace.py` with those packages, the pinned host executable,
ROM, BIOS, and the `m5-6-gba-baseline.json` report produced by the game-side
`check_backend_parity.py`. The runner starts isolated headless host processes
through the existing TCP debug interface for package enabled, disabled, absent,
and wrong-target modes. It compares the guest scene fields, checks host frame and
audio status, exercises a native host savestate restore, and writes a host trace
that the game-side checker can compare using `--host-trace`.

For example, with the generated host at `build-m5-3-verification` and both
package archives under `build/host-media/m5-3-packages`:

```powershell
python ./games/emerald_adult/tools/run_host_media_trace.py `
  --workspace-root M:/other-projects/pokemon-projects `
  --host ./games/emerald_adult/build-m5-3-verification/EmeraldAdultHost `
  --package-meadow ./build/host-media/m5-3-packages/neutral_meadow.gbamod `
  --package-coast ./build/host-media/m5-3-packages/neutral_coast.gbamod `
  --rom ../pokeemerald-modern-adult/pokeemerald_modern.gba `
  --bios ./bios/gba_bios.bin `
  --gba-baseline ../pokeemerald-modern-adult/tools/validation/.local/reports/m5-6-gba-baseline.json `
  --out ./build/host-media/m5-6-host-trace.json
```

The host test uses the framework's validation-only `GBARECOMP_CHEATS` frame
hook to replay captured 32-byte scene-state records; it does not write guest
state through the production media plugin. The guest reference reports remain
the authority for real scene progression, gallery unlock timing, and savedata.
The host trace records the ROM and host executable hashes so it must be
regenerated after either build changes.

## Generation record and observed coverage

The checked-in `emerald.toml` pins ROM SHA-1
`57d3561262bf5337293a2d31f6b79b5879f7cff7`, BIOS SHA-1
`300c20df6731a33952ded8c436f7f186d25d3492`, the explicit ARM `IntrMain` root,
Flash1M saves (128 KiB), and real BIOS execution. RTC is detected from the
cartridge's Seiko SDK signature; `RECOMP_RTC_EPOCH` fixes it for repeatable
development runs.

With the section-based M0-4a overlay and verified CLI above, generation
produced 735 BIOS functions and 119,231 cartridge functions (354 ARM, 118,877
Thumb), in 64 adaptive shards. The generator found 63,639 indirect entries and
538,628 branch targets, kept 19,291 of 150,845 speculative literal seeds, and
honored the two ELF section-derived data ranges. It emitted 33,228 named data
symbols and no code-copy annotations. Cartridge output is about 888 MB under
`generated/`; BIOS output is under `generated_bios/`. The 14,609-range mapping
symbol overlay was also evaluated, but generation correctly rejected it after
finding 114 control-flow targets inside `$d` ranges (including `0x08011D60`
from `MgbaAssert`). It is not used until those code/data conflicts are resolved.

The checked-in `tools/title-start.input.csv` replays the verified cold-start
Start presses. The headless host completed 780 frames and displayed the New
Game/Option title menu. The verified capture SHA-256 is
`89b3a8e244d9918144a1de8be306a4a313df23350d33e039971c3ccdea773960`. The run
reported 26 healed native entries and 22 dispatch misses handled by the enabled
interpreter bridge; it did not claim full static coverage. Screenshot and full
runtime logs are local ignored artifacts.

## Docker build, generation and title-menu check

The pinned Linux image supplies CMake, Ninja, Clang 14.0.6, and SDL2 2.30.11
with dummy video/audio drivers. From the workspace root, build the image and
the recompiler CLI:

```powershell
$workspace = (Get-Location).Path
Push-Location ./gbarecomp
docker build -f games/emerald_adult/Dockerfile -t emerald-adult-host-build .
Pop-Location
docker run --rm -v "${workspace}:/workspace" -w /workspace/gbarecomp `
  emerald-adult-host-build bash -lc `
  'cmake -S . -B build-docker-cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-docker-cli --target gba_recompile --parallel 8'
```

Generate the BIOS and ROM into separate ignored output directories:

```powershell
docker run --rm -v "${workspace}:/workspace" -w /workspace/gbarecomp `
  emerald-adult-host-build bash -lc `
  './build-docker-cli/gba_recompile --bios /workspace/gbarecomp/bios/gba_bios.bin --config /workspace/gbarecomp/bios/gba_bios.toml --out /workspace/gbarecomp/games/emerald_adult/generated_bios && ./build-docker-cli/gba_recompile --rom /workspace/pokeemerald-modern-adult/pokeemerald_modern.gba --symbols /workspace/gbarecomp/games/emerald_adult/symbols/imported_symbols.tsv --data-symbols /workspace/gbarecomp/games/emerald_adult/symbols/imported_data_symbols.tsv --config /workspace/gbarecomp/games/emerald_adult/emerald.toml --config /workspace/gbarecomp/games/emerald_adult/symbols/POKEMON_MODERN_EMERALD_symbols.toml --out /workspace/gbarecomp/games/emerald_adult/generated'
```

Configure, build, and run the deterministic title-menu route. The run uses an
isolated `/tmp` save and writes an ignored PNG for visual inspection:

```powershell
docker run --rm -v "${workspace}:/workspace" -w /workspace/gbarecomp/games/emerald_adult `
  emerald-adult-host-build bash -lc `
  'cmake -S . -B build-docker-m0-4 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DGBARECOMP_GENERATED_BIOS_DIR=/workspace/gbarecomp/games/emerald_adult/generated_bios && cmake --build build-docker-m0-4 --target EmeraldAdultHost --parallel 8 && RECOMP_RTC_EPOCH=946684800 GBARECOMP_INPUT_REPLAY=/workspace/gbarecomp/games/emerald_adult/tools/title-start.input.csv SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build-docker-m0-4/EmeraldAdultHost --no-window --frames 780 --save /tmp/emerald-adult-m0-4.sav --dump-png /workspace/gbarecomp/games/emerald_adult/host-title.png'
```

The first cold-start route may populate the local self-heal cache; later runs
reuse it. Interpreter fallback and self-healing stay enabled. The Docker check
does not replace checking the visible Windows window with the Windows toolchain
above.

## Local user files

The defaults in `emerald.toml` resolve these paths relative to this README:

- `../../../pokeemerald-modern-adult/pokeemerald_modern.gba` — ROM built by
  `make modern` in the sibling game repository. Override it with `--rom` when
  running from a different checkout.
- `../../bios/gba_bios.bin` — local copy of the user-provided BIOS dump.
- `local/saves/` — disposable and player cartridge saves.

The BIOS, generated code, and local saves are ignored by Git. The ROM and BIOS
are not distributed with this project.
