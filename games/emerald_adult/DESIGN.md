# Native scene media seam and runtime contract (M5-1/M5-2)

## Decision

Use a read-only, symbol-located guest state record sampled by the host at the existing `RunOptions::extended_view_frame` callback. This callback runs at emulated frame start on the game thread, so it sees a coherent guest state before the next PPU render. It reads IWRAM through `gbarecomp::active_bus()` and publishes an immutable native 240x160 BGR555 frame through the existing trusted foreign-background API. No guest function is replaced and no guest RAM is written by the observer.

The first prototype maps the neutral coast loop (scene `0x4001`, media frames `0x4110..0x4112`). The ROM continues running and remains responsible for input, timing, scene exits, unlocks, and GBA audio. The host view changes only when a valid active frame is visible. Its static sample frames are initialized before emulation; the per-frame callback performs bounded validation and publishes only on a frame, invocation, or savestate-epoch change.

## Guest ABI

`gGbaHostSceneState` is a 32-byte, 4-byte-aligned IWRAM object. ABI version 1 is pointer-free and uses fixed-width fields:

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | `u16` | version | `1` |
| 2 | `u16` | size | `32` |
| 4 | `u16` | sequence | odd during write; stable even value when readable |
| 6 | `u16` | flags | active, first frame visible, loop mode |
| 8 | `u32` | invocation ID | increments after each successful scene start |
| 12 | `u16` | scene ID | stable ID from `constants/scene.h` |
| 14 | `u16` | media ID | stable displayed frame ID |
| 16 | `u16` | frame index | index within the scene |
| 18 | `u16` | player state | `ScenePlayerState` value |
| 20 | `u32` | elapsed VBlanks | scene timeline, including completed loops |
| 24 | `u32` | total VBlanks | one scene-loop duration |
| 28 | `u32` | loop count | completed repeats |

The record adds 32 bytes. The scene-player's loop counter adds 4 bytes. The production map measured IWRAM at 26,432 of 32,768 bytes (6,336 bytes free), a 36-byte increase from the prior 26,396-byte build; EWRAM remained 261,304 bytes. The agent-test build measures IWRAM at 26,472 bytes. The prototype's three immutable 240x160 BGR555 host frames occupy 230,400 bytes of native host storage; they do not consume guest RAM. The layout probe and generated metadata reject size/offset drift. The native pipeline resolves the address only from the matching `imported_data_symbols.tsv`, and checks symbol name, IWRAM region, alignment, and exact size. The generated header is tied to the imported table hash; the host never uses an arbitrary RAM address.

The sequence guard lets the host reject an incomplete or changing record. The current callback is on the same emulation thread at a frame boundary, so it is defensive validation rather than cross-thread synchronization. Unknown versions, sizes, flags, IDs, durations, or frame mappings fail closed to the ROM presentation.

## Lifecycle and isolation

- Start: a successful allocation starts a new invocation; invalid, busy, or failed-allocation requests do not increment it.
- Playback: the guest computes elapsed VBlanks from the scene frame durations and remaining duration. Host frame selection follows `mediaId` and does not advance from wall-clock time.
- Loop/exit: the guest's loop policy, A/B input, and state machine remain authoritative. Return cleanup publishes inactive state and preserves the invocation ID for matching.
- Savestate: the IWRAM record restores with the guest. The host observes `state_epoch` and republishes the restored frame; host presentation data is not saved as guest state.
- Reset/activation: trusted runtime activation clears published presentation before callbacks. Without the selected `pokemon-adult.gallery-media-seam.v1` plugin, the observer does not publish.
- Pause/fast-forward: playback position comes from guest VBlanks, so host frames pause and accelerate with the guest.
- Netplay: optional local media remains inactive; the GBA scene is the shared deterministic fallback.
- Audio: selected activation registers the public trusted pull-stream API and starts a bounded 65536-Hz mono S16 silence source. Silence leaves guest M4A and sound effects audible at native gain 100 while proving that the committed-plugin capability is available. The observer stops the stream for invalid/inactive guest state and starts it again when a valid active scene is observed. On savestate restore, the runtime stops delivery and restores native gain but preserves the callback and capability; the changed `state_epoch` makes the observer restart it for the restored active scene. Reset retires the capability before the reset callback, which drops its local handle without using it.
- Stream callbacks run on the emulation PCM producer thread, never SDL's consumer callback. This callback fills only the caller's requested bounded buffer (at most 1024 samples), and performs no allocation, locking, I/O, SDL calls, or API re-entry. It returns the full request so ordinary buffer boundaries do not end the voice. The immutable BGR555 frame arrays are initialized before emulation and published on the game thread; the compositor consumes the published pointer as immutable data. The initial silence shell is intentionally neutral; M5-3 supplies prepared host audio and its gain policy.

## Runtime API reuse (M5-2)

No generic framework API change was needed. The M5-1 observer already runs in
the selected game host and has access to the committed-plugin presentation
gate. `gba_mod_publish_foreign_background` supplies the immutable frame path;
`gba_mod_audio_stream_register_s16_mono` provides an activation-scoped opaque
capability and `play`/`stop` handle stream lifecycle. The stream's callback
bound and gain-reset behavior are part of `docs/MOD_AUDIO.md`. When no package
is selected and committed, activation does not call the scene plugin, so there
is no stream registration or output.

No function-entry hook is needed for the production seam. The framework hook API is a replacement API with a strict CPU return-state contract; using it would add guest control flow without improving the observation. The generic hook mechanism's static, interpreted, and healed-code tiers remain unclaimed by this task.

## Verification record

The full `build-exe.ps1 -RegenerateIdentity` pipeline passed on 2026-10-26. It pinned ROM SHA-1 `163b456619b89a1b167bd2d06b3527d6c9f2c131`, imported matching ELF/map symbols, generated the scene-state address `0x03003A94`, built the native host, and passed its one-frame startup smoke. The executable SHA-256 is `8aaa0519d78bf8cfc9dc0e34ba5b653e5804a4d98d1a6661284efd3a0c759fc1`; the startup capture SHA-256 is `431a43a433136dfd520fc2c5c14b4a8bbebe4f8723cd7e449bd0221dbc5a807b`.

The pinned mGBA scene profile `20261026T120201Z-verify-08612fef` passed smoke and two scene-player runs. The ABI checker read matching agent-test IWRAM symbols and confirmed invocation 1 for the once scene, exact scene-state restoration after a savestate load, invocation 2 and one completed loop for the coast scene, then inactive state after exit. Both runs produced the same timeline.

With no mod catalog or selected package, the final host ran the deterministic 780-frame title route with `GBARECOMP_SELFHEAL_RECOMPILE=0` and a 2 GiB container limit. It completed in 1.289 seconds (about 605 guest FPS), produced the title capture, and reported no unmapped accesses. Seventeen static-coverage gaps used the Stage-1 interpreter bridge (4,746,181 interpreted instructions); this preserves the ROM fallback. A separate default self-heal attempt reached 14.6 GiB during this route and was stopped before exhausting host memory. Dynamic self-heal memory use remains a limitation for longer host runs.

The observer is read-only and its disabled path returns before reading guest memory or publishing pixels. No active-scene host replacement trace is claimed here: the GBA ABI/timeline/restore trace is qualified under mGBA, while package-selected host playback belongs to M5-3. Package absence leaves the host's ROM renderer in control.
