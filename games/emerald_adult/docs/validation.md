# Emerald Adult host validation baseline (M0-5)

This is a bring-up baseline for the optional native host. It checks the
opening route through a real trainer battle, an in-game Flash save, a fresh
process restart, and Continue. It does not qualify the full campaign or claim
strict-static execution.

## Identities

| Input | Identity |
| --- | --- |
| Fork ROM | SHA-1 `57d3561262bf5337293a2d31f6b79b5879f7cff7` |
| GBA BIOS | SHA-1 `300c20df6731a33952ded8c436f7f186d25d3492` |
| Host builder | Docker image `emerald-adult-host-build`, ID `sha256:b430bdb3b51702ea34affbe38782e8d3050ed2e458954c2f2fb278bb5ef6bcdd` |
| Host generator config | `emerald.toml`; 64 generated cartridge shards plus BIOS output |
| Save hardware | Flash1M, 128 KiB |
| GBA comparison | mGBA 0.10.5 (`26b7884bc25a5933960f3cdcd98bac1ae14d42e2`), M0-13 report `20261025T073919Z-de4b27230ded` |
| Host executable | SHA-256 `2176fd4068910723aa7b82b0088ea965de03056c562f11e19af36737b0f1fcc9` |

The host was built and run from the pinned Docker image. The host ROM and BIOS
are local inputs; neither is bundled with the host project. The GBA comparison
uses the same fork ROM SHA-1, real BIOS mode, and fixed RTC epoch.

## Route and result

The host completed cold boot, title, New Game and naming, the truck and home
interiors, Birch's lab, the Route 101 rescue and starter handoff, Oldale, Route
103, and May's trainer battle. Torchic defeated May's Mudkip. The result screen
showed Torchic at level 6 with 3/21 HP, and the decoded Route 103 defeated-rival
flag was set.

The game was saved from its in-game menu. The host reported
`save_flushed ... size=131072`; the game displayed “saved the game.” A new host
process then reported `save_loaded ... size=131072/131072`. After cold boot, the
title menu showed Continue with player name `Haaaaaa`, play time `0:25`, and
zero badges. Selecting Continue returned to Route 103 at `(10,4)`. The loaded
state had one level 6 Torchic at 3/21 HP and the Route 103 defeated-rival flag
set, confirming the save preserved the trainer result.

The host was compared with the M0-13 mGBA baseline at two shared milestones:

* After the opening reached Birch's lab, both states decoded as map `(1,4)`,
  saved position `(6,12)`, no party, and no active battle. The tracked intro,
  rescue, Pokédex, Pokémon-obtained, and Route 103 rival flags agreed; the wall
  clock initialization flag was set in both.
* At the initial Route 101 battle, both were on map `(0,16)` in battle mode
  `20`, with species `280` level 5 against species `288` level 2. The mGBA
  snapshot had HP `20/20` and `13/13`; the host snapshot had `19/19` and
  `14/14`. No explicit RNG seed was used, so this HP difference is recorded as
  RNG-dependent rather than treated as a host mismatch. The M0-13 baseline also
  independently completed its battle/save/restart route twice with matching
  canonical state, framebuffer and Flash save artifacts.

The later Route 103 trainer battle was qualified on the host. The existing
M0-13 GBA route does not contain that same trainer encounter, so no exact
frame-by-frame Route 103 comparison is claimed.

## Input, RTC, audio, and execution coverage

* **Input:** frame-indexed input replay drove menu confirms and route movement;
  saved checkpoints were inspected by screenshot and decoded map/position.
  Physical controller behavior was not evaluated in this headless run.
* **RTC:** `RECOMP_RTC_EPOCH=946684800` fixed the host clock to 2000-01-01 UTC,
  matching the M0-13 fixed-RTC baseline. The saved Continue screen reported
  the expected accumulated play time. Wall-clock RTC progression was not
  independently qualified.
* **Audio:** runs used `SDL_AUDIODRIVER=dummy`. The emulated route completed
  without an audio-device error, but this backend provides no audible quality
  check or host PCM comparison. M0-13 has the GBA PCM capture; native-host
  audio quality remains an explicit follow-up.
* **Interpreter/healing:** gameplay was run with `GBARECOMP_FORCE_INTERP=1`
  because an earlier static IRQ-driven battle run did not make progress. The
  validation logs report `coverage=NOT_STATIC`; forced-interpreter runs report
  `dispatch_misses=0`, `native_calls=0`, and 146 self-healed entries. These
  results qualify interpreter gameplay only, not static performance or broad
  translated coverage. Keep the interpreter fallback enabled.

## Reproduction commands

From the workspace root, build the pinned host target:

```powershell
$workspace = (Get-Location).Path
docker run --rm -v "${workspace}:/workspace" -w /workspace/gbarecomp/games/emerald_adult `
  emerald-adult-host-build bash -lc `
  'cmake --build build-docker-m0-4 --target EmeraldAdultHost --parallel 8'
```

Run any saved gameplay checkpoint with the deterministic environment and a
PPU-clock input trace. Replace the checkpoint, trace, and frame limit with the
recorded values for the segment being reproduced:

```powershell
docker run --rm -v "${workspace}:/workspace" -w /workspace/gbarecomp/games/emerald_adult `
  -e RECOMP_RTC_EPOCH=946684800 `
  -e GBARECOMP_FORCE_INTERP=1 `
  -e GBARECOMP_INPUT_REPLAY=/workspace/gbarecomp/games/emerald_adult/tools/m0-5-route103.input.csv `
  -e GBARECOMP_INPUT_REPLAY_CLOCK=ppu `
  -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy `
  emerald-adult-host-build bash -lc `
  './build-docker-m0-4/EmeraldAdultHost --no-window --frames 240 --load-state /workspace/gbarecomp/games/emerald_adult/local/saves/m0-5-route103-rival-tile.state --save /workspace/gbarecomp/games/emerald_adult/local/saves/m0-5-repro.sav --dump-png /workspace/gbarecomp/games/emerald_adult/host-m0-5-repro.png --dump-state /workspace/gbarecomp/games/emerald_adult/local/saves/m0-5-repro.state'
```

The restart/load check used a new process with the same Flash file (no host
state restore), then selected Continue. Its local evidence is under
`games/emerald_adult/local/saves/` and the `host-m0-5-*.png` captures beside
this document; these runtime artifacts are intentionally ignored by Git. The
saved Flash file SHA-256 was
`0580d5b2ce8c0b8f74ae6ff2ae0fa6506ffca9d2e717b3fc02fff036bca80ab8`.

## Remaining limits

This baseline does not cover the full campaign, persistent RTC advancement,
physical input, audible host output, or static/native CPU coverage. The host
and GBA share an exact early lab checkpoint and matching Route 101 encounter
species/levels, but their unseeded HP values differ as recorded above. The
Route 103 trainer win and its persistence were verified on the host through
the in-game Flash save and a fresh-process Continue.
