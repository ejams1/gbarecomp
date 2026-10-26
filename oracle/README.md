# mGBA reference oracle

The oracle is a separate reference process. It is not linked into the native
game host. The required validation harness builds it from mGBA **0.10.5** at
commit `26b7884bc25a5933960f3cdcd98bac1ae14d42e2`; do not follow a moving tag
or copy the libmgba dependency into the host build.

## Dependency identity and build status

`setup-mgba.sh` currently clones tag `0.10.5` with `--depth 1`, which does not
prove the requested commit identity. It applies two Windows-only patches:

| Patch | SHA-256 | Purpose |
| --- | --- | --- |
| `patches/0001-msys2-as-windows.patch` | `D170E67F618604AB7684099969D9AA6679CBB8A9A582CE9AFCB52A7B76458675` | Make upstream CMake take its Windows configuration under MSYS2. |
| `patches/0002-windows-no-sigtrap.patch` | `B6EBB56E78C6A7E7ED0E735635AF5DB096CE362D12877BE25D284C5F629F959F` | Remove the Windows pthread SIGTRAP path. |

The setup script SHA-256 recorded during M0-8 is
`C56DC494E5B4A0779334504BD61567046115037798D3833561A64CE45EEAACEA`. Source
hashes from that earlier audit are not current after M0-10; use Git history and
the pinned source commit for provenance, and record generated binary hashes in
run artifacts.

The legacy setup helper supports a local MSYS2/MinGW build, but it does not
enforce the full commit or pin packages. The container is the reproducible
path. `CMakeLists.txt` works as an opt-in subdirectory or a standalone
project and links the prebuilt static `${MGBA_ROOT}/build/libmgba.a` plus
matching headers. The Windows link set includes `ws2_32 shlwapi winmm ole32
uuid`; Linux links Threads, zlib, libpng, `dl`, and `m`. libmgba remains
outside every production native target.

## M0-9 build path

The selected reproducible path is the dedicated Linux container in
`Dockerfile`. It pins mGBA to commit
`26b7884bc25a5933960f3cdcd98bac1ae14d42e2`, uses Debian bookworm packages from
snapshot `20261024T000000Z` (main, updates, and security), verifies the Windows patch hash manifest, and
configures the oracle standalone. It does not configure ROM translation or the
native game host. `dependencies.md` records dependencies, exact commands,
loopback publication and the isolated-input policy.

The Windows host has Docker CLI 29.9.2 and can reach the Docker Desktop daemon
when invoked with the approved Docker command permission. WSL enumeration is
denied, but WSL is not needed for the container path. The Dockerfile uses the
Linux build and therefore does not apply Windows-only mGBA patches. The native
Windows setup helper still needs a verified source checkout before it can be
used as an alternative.

## Artifact and process contract

The Docker build keeps downloaded source and generated files in its image,
outside the game source tree. Only run inputs and reports need local host
storage:

```text
container:/opt/gbarecomp/third_party/mgba/  # pinned source and static library
container:/opt/gbarecomp/build/              # gbarecomp_oracle executable
pokeemerald-modern-adult/tools/validation/.local/ # local BIOS/ROM/scratch saves/reports
```

The game runner owns process startup/shutdown, uses an explicit config copy
based on `tools/validation/mgba.local.example.toml`, and publishes the port
only as `127.0.0.1:19843` on the host. The process listens on `0.0.0.0` only
inside the container for Docker forwarding. Never place BIOS, ROM, personal
saves, or transient reports in versioned source directories. Keep one oracle
container and scratch save per run. No native host process is needed.

## M0-8 API audit (historical baseline before M0-10)

M0-10 implemented the controls and bounds from this audit. The current
protocol, defaults, request limits, and exact endpoint list are documented in
[`controls.md`](controls.md).

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
| State | `emu_state_save` / `emu_state_load` wrap the pinned mGBA core's in-memory state API with one process-local slot, a 4 MiB cap, and opaque session IDs. | IDs are valid only in the creating oracle process; wrapper counters/audio capture are outside the saved state. See `controls.md` and `test-controls.ps1`. |
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
