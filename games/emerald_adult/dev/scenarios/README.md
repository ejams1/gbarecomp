# Host scenario recipes and state records

The required GBA harness remains the scenario source of truth. Do not copy its
action sequences into a second host scenario store. Start with the scenario
JSON and schema in
`pokeemerald-modern-adult/tools/validation/scenarios/`, then use the host
recipe here only to create a build-specific native snapshot when a matching
host route exists.

## Available recipes

| Story point | Required GBA scenario | Host recipe/evidence | Status |
| --- | --- | --- | --- |
| New game through Birch's lab, Route 101, starter rival battle, normal save, and fresh-process Continue | `m0-13-cold-start-new-game-save.json`, `m0-13-starter-rival-battle-save.json`, `m0-13-savedata-restart-continuation.json` | M0-5's BIOS host route and Flash-save/Continue procedure in `../../docs/validation.md`; frame-indexed inputs use the checked-in `tools/*.input.csv` format. | Available on the M0-5 ROM/host identity; regenerate snapshots after any ROM or framework identity change. |
| Post-gym3 | `progression-corpus.json` | No legitimate source save or source manifest exists. | Deferred; do not synthesize badges, flags, or party state. |
| Post-E4 / Hall of Fame | `progression-corpus.json` | No legitimate league-entry or completed-league save exists. | Deferred; do not synthesize story state. |
| Post-game | `progression-corpus.json` | No legitimate post-Hall-of-Fame save exists. | Deferred; do not synthesize story state. |
| Gallery-ready development state | M2/M3 feature scenarios under the GBA harness `scenarios/` directory | Use their real guest setup actions after the optional host deterministic command bridge is available (M1-4). | Deferred until M1-4 host command support is qualified. |

The host can replay a frame-indexed input file with
`GBARECOMP_INPUT_REPLAY=<path>` and `GBARECOMP_INPUT_REPLAY_CLOCK=ppu`, load
and write a native machine snapshot with `--load-state` and `--dump-state`,
and keep cartridge Flash data separate with `--save`. RTC runs use the fixed
`RECOMP_RTC_EPOCH` documented in `../../README.md`. Native snapshots are tied
to one exact ROM, BIOS, framework executable, and save schema; never call them
portable `.sav` files. The ordinary Flash save is the only portable
cross-process cartridge save interface.

## Reproducing the available early route

Use the exact ROM and host identities recorded in
`../../docs/validation.md`. Build/run the pinned host using its reproduction
commands, follow the same real BIOS input route, and capture the desired
checkpoint with `--dump-state`. For a subsequent process, load that snapshot
with `--load-state`; to test portable continuation, instead pass the same
128-KiB Flash file with `--save` and select Continue after a cold boot.

Record each generated snapshot locally under
`games/emerald_adult/local/scenarios/` (already ignored by Git). A companion
JSON record must validate against `host-snapshot.schema.json` and include the
source GBA scenario path/hash, ROM SHA-1, BIOS SHA-1, host executable SHA-256,
snapshot SHA-256/size, fixed RTC epoch, RNG policy, and the expected map,
position, relevant flags, party, and game time. Store personal saves and
machine snapshots only in ignored local directories; commit recipes and
metadata schemas, never those binary artifacts.

When an identity changes, regenerate from the route instead of attempting to
force-load an older snapshot. A matching identity is required before a local
snapshot is evidence for a current host build.
