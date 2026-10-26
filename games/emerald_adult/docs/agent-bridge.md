# Native validation TCP contract (M1-4)

The native host exposes the same line-oriented JSON control shape used by the
GBA validation harness. The host server is a transport for deterministic test
control; it is not a second scenario runner. The game repository's
`tools/validation/agent.py` remains the owner of scenario validation, bounds,
cancellation, transcripts, reports, and repeat aggregation.

## Network boundary

`--tcp PORT` binds to `127.0.0.1` by default. The optional
`--tcp-bind 0.0.0.0` is intended only when the host runs inside the pinned
Docker image and the port is published specifically to host loopback, for
example `-p 127.0.0.1:19842:19842`. `--tcp-bind` accepts only `127.0.0.1` or
`0.0.0.0`, and the latter requires `--tcp`. Do not publish this port on a LAN
interface or run an all-interface listener on an untrusted network. The
separate observer listener (`--tcp-observe`) remains loopback-only.

## Commands used by the agent adapter

Each request is one JSON object followed by a newline; each response is one
JSON object and newline. The server accepts one client at a time.

| Request | Contract |
| --- | --- |
| `{"cmd":"ping"}` | Identifies the endpoint as `gbarecomp_native`. |
| `{"cmd":"set_keyinput","value":N}` | Stores the low 10 active-low GBA `KEYINPUT` bits. The shared agent scenario uses active-high pressed masks, so the adapter complements the mask and applies `& 0x03ff`. |
| `{"cmd":"run_frames","n":N,"keyinput":K}` | Optional `keyinput` is set before stepping. Advances up to 1,000,000 real guest frames synchronously, then reports completed frames and the PPU frame counter. The agent applies its stricter per-action and scenario frame limits before sending this request. |
| `{"cmd":"write_mem","addr":A,"hex":"..."}` | Writes bytes to a bounded RAM/palette/VRAM/OAM backing range. It does not perform bus side effects. The adapter should use this only for the validation mailbox in an agent-test ROM. |
| `{"cmd":"read_ewram","addr":A,"len":N}` | Reads a bounded byte range from EWRAM and returns lowercase hex. The adapter polls the mailbox after each stepped frame. |

The 40-byte M1-5 `gAgentTestMailbox` lives in EWRAM and its address comes from
the matching ROM's generated validation metadata. The adapter writes bytes
starting at mailbox offset `+4` first, then writes the 32-bit request sequence
at offset `+0` last. It advances at least one frame and reads the mailbox back;
the guest's matching acknowledged sequence proves that the command completed
at the main-loop service boundary. `status` and `results[]` are part of the
observable result. The adapter must reject production ROMs, unavailable
symbols, malformed version/sequence, and any command not present in the
shared scenario contract before it starts stepping.

The focused CTest `tcp_debug_server_agent_contract_tests` speaks to the actual
TCP server on a temporary loopback port. A mock frame callback exercises the
wire-level mailbox ordering, acknowledgement/readback, active-low key input,
frame bounds, and RAM range checks. It verifies the default listener does not
accept a connection on a different loopback address; the opt-in all-interface
bind is kept explicit for container use.
