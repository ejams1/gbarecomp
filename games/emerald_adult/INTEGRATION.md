# Emerald Adult host integration study

**Status:** investigation only. No native game host is configured or validated by
this document. The required game path remains the pinned GBA ROM and the
headless mGBA oracle.

## Study identities

| Source | Exact identity | Use |
|---|---|---|
| EmeraldRecomp upstream | `mstan/EmeraldRecomp`, release tag `v0.1.2`, commit `b8310303011fce3fb22ad5aca50c6784affe5bd6` | Read-only scaffold reference; inspected from a clean tag checkout. |
| Upstream engine submodule at that tag | `gbarecomp` commit `215da7406f7a6d40c6d820be2d2ee778858aa598` | The reference game's engine API baseline. |
| Upstream launcher submodule at that tag | `recomp-ui` commit `0546fbcb0126bb14bf88d2128b4fe8a4229570a3` | Optional pre-boot UI dependency in the reference build. |
| Upstream decomp metadata source at that tag | `pret/pokeemerald` commit `c2dcc629fd97b3d58fedc07c9e02653eabed9136` | Reference provenance only; do not use its C sources or artifacts. |
| This workspace engine | `gbarecomp` `milestone/m0-baseline` at `6bed82e96865eb5e174b3248771247090644dff9` | Actual framework checkout for any future local host work. |
| This workspace game input | `pokeemerald_modern_adult/pokeemerald_modern.gba`, SHA-1 `57d3561262bf5337293a2d31f6b79b5879f7cff7` | Current fork ROM identity from the M0-13 qualified fixture manifest. |
| User-provided GBA BIOS | SHA-1 `300c20df6731a33952ded8c436f7f186d25d3492` | Matches `gbarecomp/bios/gba_bios.toml`; keep the file outside Git. |

The upstream release page identifies v0.1.2 as commit `b831030…`; its source
checkout records the full commit and the three gitlink revisions above. The
workspace engine is a separate checkout and is not silently replaced by the
release's submodule version.

## What the upstream project actually builds

At the pinned revision, `EmeraldRecomp/CMakeLists.txt` adds the engine with
`add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)`. Its
project-owned `add_gba_variant()` function creates desktop target
`EmeraldRecomp` (Android target `main`) and compiles `src/main.cpp` together
with the generated cartridge shards. The executable links
`gbarecomp_runtime`, `gbarecomp_debug`, `gbarecomp_gba`, and
`gbarecomp_armv4t`; desktop netplay optionally adds `gbarecomp_netplay`.
`src/main.cpp` supplies the host entry point, builds a `gbarecomp::RunOptions`,
sets the game-specific RAM dispatch hook, and calls
`gbarecomp::run_game(argc, argv, opts)`.

The executable's game host is separate from the cartridge output. The upstream
CMake target consumes `variants/emerald/generated/recompiled_NNN.cpp` shards,
`dispatch_table.cpp`, and `symbol_map.cpp` when generated. It does not make the
static library emitted by the CLI into a playable executable by itself.

The upstream CMake also offers a pre-boot `game_launcher_ui` target. It is
optional to the game entry point but enabled in that release's default
configuration. It includes the `recomp-ui` checkout, uses
`recomp_target_launcher_ui(game_launcher_ui CONSOLE gba)`, requires SDL2, and
links `opengl32` on Windows. The launcher is a dependency choice, not a
requirement for a minimal host executable.

## Workspace CMake and runtime surface

The local `gbarecomp` checkout has concrete framework targets:

- `gba_recompile` — ROM/BIOS analysis and code generation CLI.
- `gbarecomp_runtime`, `gbarecomp_debug`, `gbarecomp_gba`, and
  `gbarecomp_armv4t` — shared runtime libraries used by the upstream host.
- `gbarecomp_netplay` — optional network runtime, enabled by its CMake options.
- `gbarecomp_add_runtime_target()` in generated `build/runtime.cmake` — helper
  that accepts `GAME_GENERATED_C` or `GAME_GENERATED_DIR`, creates a target,
  and links `gbarecomp_runtime` and `gbarecomp_debug`.
- `gbarecomp::RunOptions` and `gbarecomp::run_game()` in
  `src/runtime/runtime.h` — host startup API.

The helper does **not** supply a game `main()` or the game's identity defaults.
A host must still add its own entry point, compile the correct generated
`dispatch_table.cpp`, and set the ROM/BIOS identity and runtime options. The
upstream project's `add_gba_variant()` is project code, not an engine CMake
function. M0-3 should choose either a small project-owned target around these
local APIs or a verified direct CMake composition; it must not assume an
`EmeraldRecomp` target already exists in this workspace.

The current engine builds the SDL-backed window and input implementation only
when SDL2 is available; otherwise the runtime's window code has a no-SDL stub.
A visible host target therefore needs an explicit SDL2 dependency and a
successful window-open check. Framework build guidance currently names CMake
3.20+, and the upstream Emerald release uses MSYS2 MinGW, Ninja, and SDL2. The
GBA ROM toolchain remains separate and pinned to devkitARM r63 / GCC 13.2.0.

## Startup, identity, saves, clock, and diagnostics

### Startup and dispatch

The upstream main parses the host arguments, initializes mobile host setup when
applicable, installs its RAM-dispatch hook, prepares a `RunOptions` object, and
then calls `gbarecomp::run_game()`. Both BIOS and cartridge ROM are required by
the runtime path. The runtime verifies their configured SHA-1 values before
execution; the launcher and host options may prefill paths but do not bypass
those identity checks.

The upstream `gba_recompile` emits the BIOS dispatch/code and cartridge
functions plus a cartridge dispatch table. BIOS output defaults to
`src/runtime/generated_bios`; cartridge output defaults to the selected
`--out` directory. The BIOS is generated separately with `--bios` and its
pinned `bios/gba_bios.toml`; the game uses `--rom`, an explicit program config,
function seeds, and any reviewed overlays. Dispatch data must be linked from
the generated output corresponding to the exact ROM and config identity.

The workspace engine also exposes `GBARECOMP_GENERATED_BIOS_DIR` as a CMake
cache path. If BIOS generation uses a build-local output directory, the host
configuration must point the engine at that same directory so
`gbarecomp_runtime` compiles `bios_dispatch_table.cpp` and
`bios_recompiled.cpp`; merely generating BIOS files elsewhere does not link
them.

### ROM and BIOS identities

The upstream Emerald release targets stock USA Emerald SHA-1
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`. Its `game.toml` and region
config are not valid identities for `pokeemerald_modern.gba`, whose qualified
workspace SHA-1 is `57d3561262bf5337293a2d31f6b79b5879f7cff7`. The host's built-in
SHA-1, base recompiler config, any symbol overlay, and runtime `[rom].sha1`
must all describe the same fork ROM. Hash drift requires explicit regeneration
and a derived identity update.

The pinned BIOS config expects SHA-1
`300c20df6731a33952ded8c436f7f186d25d3492`. The local BIOS is user-supplied;
its identity is checked at runtime and its binary must not be copied into the
repository or a release. The default LLE path uses the real BIOS. Any future
BIOS HLE selection must remain build/config-selected and retain the working
LLE path.

### Flash and RTC

Upstream Emerald config declares `flash1m` and 128 KiB (`0x20000`) saves. The
local runtime can also detect the ROM's save signature, but the game config
should state the measured hardware explicitly. Normal battery saves default to
`<rom>.sav`; `--save`/`--save-path` or `[save].path` selects an explicit path.
Host validation must point that path into a scratch directory and test ordinary
cartridge save persistence separately from emulator savestates.

The local RTC recognizes the Seiko S-3511A signature and supports a fixed test
clock with `RECOMP_RTC_EPOCH`. Tests must set this explicitly for repeatability;
player builds should not freeze the clock accidentally. The Emerald RTC changes
in the upstream release are title-specific and are not proof that the fork's
RTC path works in a native host.

### Diagnostics

The local runtime's game config supplies a debug TCP port, with command-line
and runtime overrides available. Self-heal output is per ROM: coverage JSON
and a proposed `recomp_master_misses_<tag>.toml.frag`; the proposal is not
silently merged into game identity. Session diagnostic replays are segmented
`.replay` files with sidecars. `GBARECOMP_DIAGNOSTICS_DIR` chooses their
location, `GBARECOMP_DIAGNOSTICS=0` opts out, and
`tools/replay_diagnostic.py check|replay` inspects or replays a recording.
Diagnostic output and save paths must be isolated from source and user data.

## Symbol import and regeneration plan for the fork

The upstream repository imports only symbol names, function boundaries and
ROM-layout annotations from a byte-matching decomp build. It does not use the
decomp's C source as an execution oracle. Follow the same boundary, but import
metadata from the exact modern fork ELF that produced the current ROM:

1. Build `pokeemerald_modern.gba` and its matching `.elf` and `.map` with the
   pinned game Docker image. Record the ROM SHA-1 and ELF/map identities.
2. Capture text from the matching ELF with `arm-none-eabi-readelf -sW` and
   `-SW`. Use the matching link map and ROM. The raw game `.sym` file is not
   the importer's function TSV.
3. Run
   `gbarecomp/tools/symbol_import/import_decomp_symbols.py` with `--syms`,
   `--sections`, `--map`, `--rom`, and `--out`. It emits
   `imported_symbols.tsv`, `imported_data_symbols.tsv`,
   `function_boundaries.tsv`, and a ROM-identity overlay TOML.
4. Keep a hand-owned base game config with `[program]`, `[identity]`, and
   measured `[save]` settings. Pass that first to `gba_recompile`; pass the
   importer overlay and any reviewed seeds as later `--config` overlays. Pass
   `imported_symbols.tsv` as `--symbols` and
   `imported_data_symbols.tsv` as `--data-symbols`. Emit to ignored generated
   storage and link the resulting shards and dispatch table into the host.
5. Generate BIOS output separately from the user BIOS and pinned
   `bios/gba_bios.toml`. Record BIOS input hash and generated metadata identity.
   Never edit generated C/C++ manually.

The framework build starts in the `gbarecomp` checkout. The symbol and
generation commands run from `pokeemerald-modern-adult` after the pinned Docker
build has produced the matching ROM, ELF, and map. These are commands for
M0-3/M0-4 to validate against the current checkouts; this study does not claim
they have already built the fork host:

```powershell
# From the gbarecomp root:
cmake -S . -B build -G Ninja
cmake --build build --target gba_recompile --config Release --parallel
```

```powershell
# From the pokeemerald-modern-adult root:
$engineRoot = "..\gbarecomp"
$hostRoot = Join-Path $engineRoot "games\emerald_adult"
$symbolDir = "tools\validation\.local\symbol-import"
$recompiler = Join-Path $engineRoot "build\gba_recompile.exe"
$importer = Join-Path $engineRoot "tools\symbol_import\import_decomp_symbols.py"
$biosConfig = Join-Path $engineRoot "bios\gba_bios.toml"
$biosPath = "D:\path\to\user-provided\gba_bios.bin"

# Capture from the matching ELF in the pinned build container. Container-side
# redirection preserves readelf's ASCII output and the chained exit status.
docker run --rm -v "${PWD}:/work" -w /work adult-emerald-build:r63-libpng `
  sh -c "mkdir -p tools/validation/.local/symbol-import && arm-none-eabi-readelf -sW pokeemerald_modern.elf > tools/validation/.local/symbol-import/readelf-symbols.txt && arm-none-eabi-readelf -SW pokeemerald_modern.elf > tools/validation/.local/symbol-import/readelf-sections.txt"
python $importer `
  --id POKEEMERALD_MODERN --name "Pokemon Modern Emerald (USA)" `
  --syms (Join-Path $symbolDir "readelf-symbols.txt") `
  --sections (Join-Path $symbolDir "readelf-sections.txt") `
  --map pokeemerald_modern.map --rom pokeemerald_modern.gba --out $symbolDir

# BIOS generation is separate and requires the user-provided binary:
& $recompiler --bios $biosPath `
  --config $biosConfig --out (Join-Path $engineRoot "build\generated_bios")

# Cartridge generation, after the host's base config and importer output exist:
& $recompiler --rom pokeemerald_modern.gba `
  --symbols (Join-Path $symbolDir "imported_symbols.tsv") `
  --data-symbols (Join-Path $symbolDir "imported_data_symbols.tsv") `
  --config (Join-Path $hostRoot "emerald.toml") `
  --config (Join-Path $symbolDir "POKEEMERALD_MODERN_symbols.toml") `
  --out (Join-Path $hostRoot "generated")
```

Readelf redirection must preserve text bytes and the native exit status. M0-4a
will determine the safe capture invocation for the pinned Windows shell and
verify importer warnings before this recipe is treated as proven.

## Upstream sample drift and concrete risks

- The pinned upstream README says to pass
  `variants/emerald/symbols/emerald_usa.toml`, but that file is absent at
  `v0.1.2`; the checked-in outputs are named `BPEE_symbols.toml` and
  `BPEE_reviewed_seeds.toml`. Its hand-authored `variants/emerald/game.toml`
  says it is the base config and the symbol files are overlays. Do not copy the
  stale README command or overwrite the base config during import.
- The pinned upstream `variants/emerald/generated/README.md` describes a
  monolithic `recompiled.cpp`, while its CMake function rejects that file and
  expects the current sharded `recompiled_NNN.cpp` layout. The checked-in CMake
  and generator configuration, not that generated-directory note, govern the
  scaffold.
- Upstream `README.md` says CMake 3.16+, but its pinned `CMakeLists.txt`
  requires 3.20. Use the stricter project minimum.
- The upstream repo pins its engine and launcher submodules by gitlink, but its
  Windows guide does not pin MSYS2, MinGW, Ninja, or SDL2 package versions.
  M0-3 must choose and record the actual host toolchain versions before calling
  the native build reproducible.
- The upstream submodule engine revision `215da740…` differs from the local
  engine revision `6bed82e…`. The local `gbarecomp_add_runtime_target()` API
  and generated runtime CMake file are the source of truth for M0-3; compile a
  minimal consumer before relying on integration assumptions.
- The upstream CMake uses project-owned launcher, netplay, touch, and extended
  view sources. None is needed to prove a minimal window lifecycle. Add these
  only in their own tasks after the core runtime linkage is verified.
- The fork ROM's identity and symbol addresses must come from its own matching
  ROM/ELF/map. The upstream stock-ROM SHA-1 and importer files cannot be reused.
- ROM-derived C/C++ and local BIOS/ROM/save inputs remain ignored. Only
  hand-authored integration/config/documentation belongs in the repository.

## M0-3 handoff checklist

- [ ] Add a host-owned `CMakeLists.txt`, entry point, README, and ignore rules
      under `games/emerald_adult/`; keep them outside any generated directory.
- [ ] Pin the chosen local engine integration method and any optional launcher
      dependency; do not use the upstream `EmeraldRecomp` target name by
      assumption.
- [ ] Configure and build a stub executable that opens and closes a window.
- [ ] Make missing ROM/BIOS and identity errors actionable. Store local saves
      beside the executable or in another documented user-data location; do
      not ship input assets.
- [ ] Keep actual cartridge execution explicitly unimplemented until M0-4
      supplies matching generated dispatch and identity metadata.

## References

- Upstream release/tag: <https://github.com/mstan/EmeraldRecomp/releases/tag/v0.1.2>
- Exact upstream commit: <https://github.com/mstan/EmeraldRecomp/commit/b8310303011fce3fb22ad5aca50c6784affe5bd6>
- Workspace framework target/API source: `gbarecomp/CMakeLists.txt`,
  `gbarecomp/cmake/runtime.cmake.in`, `gbarecomp/src/runtime/runtime.h`, and
  `gbarecomp/src/runtime/runtime.cpp`.
