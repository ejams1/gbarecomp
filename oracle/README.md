# mGBA reference oracle

The oracle is a separate reference process. It is not linked into the native
game host. The required validation harness builds it from mGBA **0.10.5** at
commit `26b7884bc25a5933960f3cdcd98bac1ae14d42e2`; do not follow a moving tag
or copy the libmgba dependency into the host build.

## Current source and build status

`setup-mgba.sh` currently clones tag `0.10.5` with `--depth 1`, which does not
prove the requested commit identity. It applies two Windows-only patches:

| Patch | SHA-256 | Purpose |
| --- | --- | --- |
| `patches/0001-msys2-as-windows.patch` | `D170E67F618604AB7684099969D9AA6679CBB8A9A582CE9AFCB52A7B76458675` | Make upstream CMake take its Windows configuration under MSYS2. |
| `patches/0002-windows-no-sigtrap.patch` | `B6EBB56E78C6A7E7ED0E735635AF5DB096CE362D12877BE25D284C5F629F959F` | Remove the Windows pthread SIGTRAP path. |

Oracle source SHA-256 at audit: `148D4A151082639C4655BE3A894F092A784FA2597C7BEF0F56512C00B4EA1521`
(`main.cpp`); CMake SHA-256: `4946147A4077E15EDA5EBC00D7E921C2E1ABAD6DAF7BE811B9876727B5CA7071`;
setup script SHA-256: `C56DC494E5B4A0779334504BD61567046115037798D3833561A64CE45EEAACEA`.
These are audit identities, not a substitute for the source pin or hashes of
generated binaries.

The current setup helper supports a local MSYS2/MinGW build or a Unix-like
shell, but does not pin compiler/package versions, verify the clone commit, or
isolate all build outputs. `oracle/CMakeLists.txt` expects a prebuilt static
`${MGBA_ROOT}/build/libmgba.a` and matching headers. The outer CMake target is
opt-in and must remain separate from the normal host target. The Windows link
set includes `ws2_32 shlwapi winmm ole32 uuid`; the Unix/macOS branches add
their platform libraries. Linux/WSL is not an assumed build path.

## Provisioning decision for M0-9

Use **MSYS2 MinGW-w64 UCRT64** on Windows as the reproducible target. Install
MSYS2 and the x86_64 UCRT toolchain, CMake, Ninja, Git, pkg-config, and the
headers required by the pinned mGBA 0.10.5 minimal static build. Record exact
MSYS2 package versions and archive hashes in the M0-9 lock/provisioning note;
the package database is rolling, so a package-name-only list is insufficient.
Run configure/build from the UCRT64 shell and invoke the matching CMake/Ninja
from that environment. M0-9 must amend the existing helper to verify the full
mGBA commit, verify patch hashes/application, and use pinned package inputs.

This session's Windows host has CMake 4.3.1 and Git 2.53.0, but no detected
MSYS2/MinGW compiler, Ninja, mGBA application/package, mGBA source checkout,
or oracle executable. Docker CLI 29.9.2 is installed; WSL reports
`E_ACCESSDENIED` while enumerating distributions, and container daemon/image
availability was not established. Therefore a container recipe is not a
reproducible fallback here. No local mGBA package hash can be reported because
no package is installed. Provisioning and actual package hashes remain M0-9
work; the pinned upstream source commit above is the immutable source identity.

Keep ROM compilation on the independently pinned `adult-emerald-build:r63-libpng`
image. It is not the oracle toolchain and does not imply Docker is available to
build mGBA.

## Artifact and process contract

M0-9 should keep all downloaded source and generated files ignored and outside
the game source tree:

```text
gbarecomp/third_party/mgba/                 # pinned source, ignored
gbarecomp/third_party/mgba/build/            # static libmgba + headers, ignored
gbarecomp/build/oracle/                      # gbarecomp_oracle.exe, ignored
pokeemerald-modern-adult/tools/validation/.local/ # local BIOS/ROM/scratch saves/reports
```

The game runner owns process startup/shutdown, uses an explicit config copy
based on `tools/validation/mgba.local.example.toml`, and binds only to
`127.0.0.1`. The current oracle listens on TCP 19843 by default; use a
per-run assigned port when available and fail if the endpoint is already in
use. Never place BIOS, ROM, personal saves, or transient reports in versioned
source directories. Keep one oracle process and scratch save per run. The
oracle and game runner communicate over localhost; no native host process is
needed.

## Existing API audit (observed in `main.cpp`)

The oracle currently accepts line-delimited JSON over one localhost TCP client
at a time, with an 8192-byte request cap and one-line JSON replies. It has no
structured startup metadata/health negotiation beyond `ping`, and unknown
arguments are logged rather than rejected.

| Capability | Existing behavior | Qualification gap / M0-10 assignment |
| --- | --- | --- |
| Reset | Startup loads optional BIOS and ROM, then resets. BIOS+ROM startup applies SVC/PC=0/I/F real-reset CPU state. No-BIOS+ROM startup requests mGBA's HLE BIOS skip. `emu_reset` always applies real-reset register state, even after HLE startup. | Track selected reset mode; make reset honor it and expose it. Test both modes against known boot behavior. |
| Frame stepping | `emu_step`/`emu_step_to_vblank` call `core->runFrame`; wrapper increments `vblanks` and `frame` once per call. `emu_step_frames` repeats this. | These are wrapper frame counts, not measured VBlank IRQs; rename/report honestly or observe real VBlank events. Define frame boundary and step limits. |
| Instruction stepping | `emu_step_inst` calls `core->step` once and reports registers/cycle delta. `emu_run_until_pc` and related diagnostic stepping exist. | Bound all work and report timeout/stop reason consistently. These are diagnostic controls, not a game route API. |
| Input | `emu_set_keys` forwards its integer directly to `core->setKeys`. | GBA key active-low polarity, bit mapping, edge/held semantics, and when input takes effect are undocumented/unqualified. Specify and test phase. |
| RTC | No oracle command or run clock control is exposed. | Pin deterministic RTC epoch/clock behavior or prove the fork does not consume RTC; include it in run metadata. |
| Saves | `mCoreLoadFile` followed by `mCoreAutoloadSave` may load the sibling `<rom>.sav`; absence is only a warning. There is no explicit save-path parameter, isolation guard, save-copy command, or dirty-save report. | Require per-run ROM/scratch-save copies, disable implicit access to the shared ROM directory, expose the actual save identity and persistence outcome. |
| State | There are RAM/IO/register injection and read APIs, plus PPU/DMA/timer/IRQ/audio inspection. No oracle savestate save/load command is present. | Do not claim savestate/replay support. Add it only if runner needs it, with version/ROM identity checks and tests. |
| Video/audio | Framebuffer capture is RGB888 240x160; audio tap collects mono samples at the mGBA-reported rate and can drain them. | Add capture bounds and report dimensions/rate/sample interval; verify repeatability. These are diagnostics, not currently a qualified artifact format. |
| Shutdown/errors | `quit` replies then exits server; socket/core teardown occurs on normal return. `init` failure paths can return before `shutdown`; accept/recv failures are minimally handled; malformed JSON is substring-scanned. | M0-10 must provide deterministic shutdown, startup failure propagation, client disconnect handling, strict request/response errors, and bounded command behavior. |

Additional existing inspection commands include `read_emu_{oam,pal,vram,iwram,ewram,io,rom}`, `emu_registers`, `emu_pc`, `emu_inst_count`, `emu_{matrix,ppu,dma,dma_internal,timer,audio,irq}_state`, `emu_screenshot`, and `emu_audio_samples`. `emu_set_regs` and `emu_write` are state-injection/debug interfaces; they do not establish a supported scripted game scenario by themselves.

## Acceptance and run metadata

The first acceptance level is isolated headless execution: exact ROM and BIOS
hashes, oracle source/build identity, process and config identity, unique
scratch save path, deterministic input/time setup, bounded route, exit status,
and report paths. A successful launch or TCP `ping` is not a gameplay pass.
Reference determinism and deliberate fault detection must be demonstrated
before reports qualify. The oracle is mGBA; do not introduce an assumed
`mgba-sdl --headless --script` command or dependency on the optional native
source/recompiled host.
