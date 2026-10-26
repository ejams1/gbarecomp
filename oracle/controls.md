# Oracle controls (protocol 2)

The oracle listens on TCP port 19843 and accepts one line-delimited JSON
object per request. A connection is handled serially. Requests are limited to
8192 bytes; a client that sends no complete request or stops reading is closed
after a 30 second socket deadline. Replies are newline terminated. JSON must be
a flat object with scalar values and unique property names. Escaped Unicode is
accepted only for ASCII code points; oracle command and data fields use ASCII.
Commands can use
their own boolean property (`{"emu_phase":true}`) or the established
`{"cmd":"emu_phase"}` form. Invalid JSON and invalid command arguments
return `ok:false`.

Start with a private working directory and explicit inputs:

```text
gbarecomp_oracle --rom /work/rom.gba --no-bios --port 19853 --bind 0.0.0.0
  --rtc-mode fixed --rtc-epoch-ms 946684800000
```

`--bios FILE` selects real BIOS reset; `--no-bios` selects mGBA's HLE boot
handoff. The default RTC is fixed at 2000-01-01 00:00:00 UTC. `--rtc-mode tick`
advances it using guest frame cycles. Epoch milliseconds must be within the
Unix range ending at 9999-12-31. `--help` and `--version` do not start a
server. Unknown options, invalid ports, and conflicting BIOS options fail
before startup.

## Capabilities and phase

`{"emu_capabilities":true}` reports protocol and mGBA identities plus the
supported request, frame, memory, audio, and save limits. `{"emu_phase":true}`
reports four separate observations:

* `run_frame_calls`: number of wrapper calls to `mCore::runFrame` since reset.
* `core_frame_counter`: mGBA's guest core frame counter.
* `video_frame_end_events`: actual `videoFrameEnded` callback count.
* `key_reads`: actual `keysRead` callback count.

It also reports the current positive `keys_pressed` mask, PPU `vcount`, and
selected reset mode. None of these counters is described as a VBlank IRQ
count. The legacy `emu_vblank_count` command returns an error. `frame` remains
as a compatibility alias for wrapper calls.

The currently supported protocol endpoints are `ping`, `frame`,
`emu_capabilities`, `emu_phase`, `emu_rtc_state`, `emu_set_rtc`,
`emu_state_save`, `emu_state_load`,
`emu_set_keys`, `emu_step_frames`, `emu_step`, `emu_step_to_vblank`,
`emu_reset`, `emu_save_import_begin`, `emu_save_import_chunk`,
`emu_save_import_commit`, `emu_save_export_begin`, `emu_save_export_chunk`,
`emu_save_export_end`, `emu_screenshot`, `emu_audio_samples`,
`emu_registers`, `emu_pc`, `emu_inst_count`, `emu_step_inst`,
`emu_run_until_pc`, `emu_find_dma_source_reset`, `emu_{matrix,ppu,dma,
dma_internal,timer,audio,irq}_state`, `read_emu_{oam,pal,vram,iwram,ewram,
io,rom}`, `emu_{set_regs,write}`, and `quit`. The two `emu_*` injection
commands remain diagnostic interfaces.

`videoFrameEnded` and `keysRead` callbacks increment independent observations
when mGBA emits them during guest execution. There is no total ordering
contract between those callbacks: a route should assert the observed phase
fields it needs, rather than assume a callback runs once per wrapper call.

`emu_step_frames` accepts `count` from 0 through 3600 and calls `runFrame` for
each requested frame. It does not wait for a display refresh. `emu_step` and
`emu_step_to_vblank` each make one wrapper call. Use the measured phase fields
to determine guest progress. Each low-level diagnostic step is bounded to one
instruction, and `emu_run_until_pc` / `emu_find_dma_source_reset` cap their
work at one million instructions.

## Input, RTC, reset, and temporary saves

`emu_set_keys` accepts a positive pressed-key bitmask from 0 to 1023. Bits 0–9
are A, B, Select, Start, Right, Left, Up, Down, R, and L. Held keys remain
held until the next `emu_set_keys`; the value is visible immediately, and the
guest samples it when mGBA next polls keys during execution.

`emu_set_rtc` takes `mode` (`fixed` or `tick`) and `epoch_ms`. `emu_rtc_state`
reports mGBA's RTC value and both guest and wrapper frame counters. In tick
mode, the selected epoch is the current RTC time when the command is applied;
subsequent guest cycles advance it. Reset clears phase/audio diagnostic
counters, releases all keys, preserves the selected BIOS/HLE mode, and
reapplies the current RTC mode and anchor.

The oracle never calls mGBA's sibling-save autoload helper and does not load a
user config file. Config teardown only releases in-memory tables. Save data is
private to each oracle process and is not written to disk. To import, call
`emu_save_import_begin` with a supported byte size (512, 8192, 32768, 65536,
or 131072), send sequential `emu_save_import_chunk` messages (up to 2048 raw
bytes encoded as hex), then `emu_save_import_commit`. To export the actual
cartridge savedata, call `emu_save_export_begin`, fetch the returned size in
bounded `emu_save_export_chunk` calls, then call `emu_save_export_end`.
Persistence is entirely the caller's responsibility; only a caller that owns
the scratch path should write the exported bytes there.

## In-memory emulator state

`emu_state_save` asks the pinned mGBA core to serialize its current emulation
state into one process-local buffer and returns an opaque numeric `state_id`
and its byte size. `emu_state_load` takes that ID and restores the state into
the same running core. Replacing a saved state invalidates the prior ID. The
buffer is limited to 4 MiB, is never written to disk, and is freed when the
oracle exits. Requests cannot provide file paths or state bytes. The command
is available only when `emu_capabilities.state_api` is
`mcore-in-memory-single-slot-v1`; its limit and slot count are also reported
there.

The state belongs to the one ROM loaded when the process starts. The oracle
checks that the saved byte length still matches the current mGBA core before
loading. Keep save and load within one process; IDs are not portable between
oracle sessions or mGBA versions. mGBA core state restores guest/core fields,
including CPU and frame progression, but not oracle wrapper observations such
as `run_frame_calls`, audio capture history, or diagnostic counters. Compare
`core_frame_counter` and guest memory/register observations for a round trip;
do not expect wrapper call counts to rewind. `emu_reset` leaves the saved blob
available, so a scenario can restore a checkpoint after reset if needed.

## Captures and diagnostics

`emu_screenshot` returns RGB888, 240x160, in row-major order (115200 bytes).
Memory reads are capped at 4096 bytes. `emu_write` accepts 1–2048 bytes and
aligned widths of 8, 16, or 32 bits. `emu_set_regs` validates every supplied
register as an unsigned 32-bit value before changing any register. These two
commands are diagnostic state injection; `emu_set_keys` is the gameplay input
surface.

The oracle suppresses informational/debug mGBA log messages by default because
BIOS SWI and DMA messages can be emitted repeatedly on every frame. Fatal and
error messages remain on stderr. Set `GBA_ORACLE_VERBOSE=1` in the oracle
process environment to restore the full diagnostic stream.

The audio tap retains at most 262144 generated samples in memory. When
full it drops new samples and reports `samples_dropped`; draining samples
releases queue capacity. This retains offscreen audio mixing while bounding
memory use. Screenshot and audio responses are bounded by their fixed output
limits.

From the `gbarecomp` checkout root, run the focused two-worker checks with:

```powershell
./oracle/test-controls.ps1 -RomPath "../Pokemon - Emerald Version (USA, Europe).gba"
./oracle/test-controls.ps1 -RomPath "../Pokemon - Emerald Version (USA, Europe).gba" -BiosPath "path/to/lawfully-held/gba_bios.bin"
```

The HLE invocation checks capabilities, input, phase, RTC, reset, screenshot,
save round-trip, malformed requests, and two simultaneous isolated save
workers. Supply a lawful BIOS path to qualify the real BIOS reset path as
well.
