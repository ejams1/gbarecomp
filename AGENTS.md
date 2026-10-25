# AGENTS.md — gbarecomp (+ games/emerald_adult)

This checkout hosts the static-recompilation framework **and** our game host
project at `games/emerald_adult/` (created in M0-3). Read the workspace
`../AGENTS.md` for branch workflow and the task list. The framework's own
`CLAUDE.md` and `docs/` remain authoritative for framework internals.

## Framework ground rules (from CLAUDE.md / docs/ARCHITECTURE.md)

- Fix the analyzer/emitter/hardware model, then **regenerate** — never
  hand-edit generated (sharded) C++.
- Layer separation: `src/armv4t/` (portable CPU), `src/gba/` (hardware
  models), `src/runtime/` (dispatch/host glue), `src/debug/`. Don't bleed.
- Keep a working LLE build for any HLE-replaced service; presentation-only
  changes must not feed back into deterministic simulation (netplay,
  savestates, state hashes).
- BIOS and ROM are user-supplied, hash-verified; they are not shipped.

## Our game host (`games/emerald_adult/`)

- Input: `pokeemerald_modern.gba` (from the decomp repo, `make modern`) +
  `pokeemerald_modern.gba.sym` (symbol import) + our `emerald.toml`.
- Cartridge hardware: Flash 1Mbit + RTC (the fork uses `agb_flash_1m.c`).
- ROM identity (SHA-1) is pinned in `emerald.toml`; the pipeline script
  (`build-exe.ps1`, M0-6) warns on hash drift — regenerate, then re-pin.
- Trusted plugins (gallery media backend, M5) register under stable IDs per
  `src/runtime/mod_runtime.h`; follow `docs/MOD_PACKAGES.md`,
  `docs/MOD_AUDIO.md`, `docs/FOREIGN_SCREEN_OVERLAY.md` contracts:
  - presentation seams (foreign background, screen overlay) are
    presentation-only and are cleared on savestate restore / PPU reset —
    republish after restore, never assume continuity;
  - mod-audio stream voice: one committed trusted pull callback, bounded,
    no allocations/locks/SDL inside it.
- Determinism: guest timing stays deterministic; host media/presentation is
  local per peer (netplay).

## Verification

- Framework changes: build + `ctest` (see CLAUDE.md build section).
- Host changes: `build-exe.ps1` (or manual: generate → cmake) must produce a
  working exe; validate against `games/emerald_adult/docs/validation.md`
  (created in M0-5).
