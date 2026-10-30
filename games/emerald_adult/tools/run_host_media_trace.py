#!/usr/bin/env python3
"""Run package-mode host traces against the pinned Linux host executable.

The guest scene records come from the real BIOS mGBA report. This runner writes
those captured 32-byte records at the host's documented validation frame hook,
then observes the real installed-package resolver/plugin/presentation path over
the host's loopback TCP debug interface. The validation-only GBARECOMP_CHEATS
input keeps a captured record stable across host frames and savestate tests.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tomllib
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any


PLUGIN_PACKAGE = "adult.gallery-media"
PLUGIN_FEATURE = "gallery-media"
STATE_SYMBOL = "gGbaHostSceneState"
STATE_FIELDS = (
    "scene_id", "media_id", "frame_index", "player_state",
    "elapsed_vblanks", "total_vblanks", "loop_count",
    "invocation_id", "flags",
)


def digest(path: Path, kind: str = "sha256") -> str:
    h = hashlib.new(kind)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(command: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}\n"
                           f"{result.stdout}")
    return result.stdout


def extract_package(archive: Path, destination: Path, wrong_target: bool) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        members = package.infolist()
        for member in members:
            name = PurePosixPath(member.filename)
            if (name.is_absolute() or not name.parts or
                    any(part in ("", ".", "..") for part in name.parts) or
                    "\\" in member.filename or ":" in member.filename):
                raise ValueError(f"unsafe package member: {member.filename!r}")
        package.extractall(destination)
    if wrong_target:
        manifest = destination / "manifest.toml"
        text = manifest.read_text(encoding="utf-8")
        text, changed = re.subn(
            r'(?m)^rom_sha1\s*=\s*"[0-9a-fA-F]{40}"\s*$',
            'rom_sha1 = "0000000000000000000000000000000000000000"',
            text, count=1)
        if changed != 1:
            raise ValueError("package manifest has no exact ROM target SHA-1")
        manifest.write_text(text, encoding="utf-8")


def write_mod_state(executable_dir: Path, *, installed: bool,
                    enabled: bool = True) -> None:
    mods = executable_dir / "mods"
    mods.mkdir(parents=True, exist_ok=True)
    if not installed:
        return
    state = (
        "format_version = 1\n\n"
        "[[package]]\n"
        f'id = "{PLUGIN_PACKAGE}"\n'
        'version = "1.0.0"\n\n'
        "[[feature]]\n"
        f'package_id = "{PLUGIN_PACKAGE}"\n'
        f'id = "{PLUGIN_FEATURE}"\n'
        f"enabled = {'true' if enabled else 'false'}\n"
    )
    (mods / "state.toml").write_text(state, encoding="utf-8")


def cheat_string(event: dict[str, Any]) -> str:
    # The runtime applies these bounded, RAM-only constants immediately before
    # its extended-view callback at each frame boundary.
    base = 0x03003A94
    values = [
        (0, 16, 2), (2, 16, 32), (4, 16, 2),
        (6, 16, event["flags"]), (8, 32, event["invocation_id"]),
        (12, 16, event["scene_id"]), (14, 16, event["media_id"]),
        (16, 16, event["frame_index"]), (18, 16, event["player_state"]),
        (20, 32, event["elapsed_vblanks"]),
        (24, 32, event["total_vblanks"]), (28, 32, event["loop_count"]),
    ]
    return ";".join(f"{base + offset:08X}:{bits}:{value:X}"
                     for offset, bits, value in values)


def make_container_script(executable_dir: Path, workspace: Path,
                          config: Path, rom: Path, bios: Path,
                          event: dict[str, Any], port: int,
                          restore: bool) -> Path:
    rel_exe = executable_dir.relative_to(workspace).as_posix()
    rel_config = config.relative_to(workspace).as_posix()
    rel_rom = rom.relative_to(workspace).as_posix()
    rel_bios = bios.relative_to(workspace).as_posix()
    save = (executable_dir / "trace-save.sav").relative_to(workspace).as_posix()
    state = (executable_dir / "trace-state.gbst").relative_to(workspace).as_posix()
    script = executable_dir / "run-trace.sh"
    commands = [
        "send '{\"cmd\":\"run_frames\",\"n\":2}'",
    ]
    if restore:
        commands.extend([
            f"send '{{\"cmd\":\"savestate_save\",\"path\":\"/workspace/{state}\"}}'",
            "send '{\"cmd\":\"run_frames\",\"n\":1}'",
            f"send '{{\"cmd\":\"savestate_load\",\"path\":\"/workspace/{state}\"}}'",
            "send '{\"cmd\":\"run_frames\",\"n\":1}'",
        ])
    commands.extend([
        "printf '%s\\n' '{\"cmd\":\"gallery_media_status\"}' >&3",
        "IFS= read -r response <&3",
        "printf 'TRACE_JSON:%s\\n' \"$response\"",
        "send '{\"cmd\":\"quit\"}'",
        "wait \"$pid\"",
        "trap - EXIT",
        "cat host.log",
    ])
    content = "\n".join([
        "set -euo pipefail",
        f"cd /workspace/{rel_exe}",
        "export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy",
        f"export GBARECOMP_CHEATS='{cheat_string(event)}'",
        f"./EmeraldAdultHost --config /workspace/{rel_config} "
        f"--bios /workspace/{rel_bios} --rom /workspace/{rel_rom} "
        f"--save-path /workspace/{save} --tcp {port} > host.log 2>&1 &",
        "pid=$!",
        "trap 'kill $pid 2>/dev/null || true' EXIT",
        "connected=0",
        "for attempt in $(seq 1 60); do",
        f"  if (echo > /dev/tcp/127.0.0.1/{port}) 2>/dev/null; then connected=1; break; fi",
        "  if ! kill -0 \"$pid\" 2>/dev/null; then cat host.log; exit 1; fi",
        "  sleep 1",
        "done",
        "if [ \"$connected\" != 1 ]; then cat host.log; echo 'host debug port timed out' >&2; exit 1; fi",
        f"exec 3<>/dev/tcp/127.0.0.1/{port}",
        "send() { printf '%s\\n' \"$1\" >&3; IFS= read -r response <&3; printf '%s\\n' \"$response\"; }",
        *commands,
    ]) + "\n"
    script.write_text(content, encoding="utf-8", newline="\n")
    return script


def run_case(*, name: str, mode: str, state: dict[str, Any], archive: Path,
             host_executable: Path, workspace: Path, config: Path,
             rom: Path, bios: Path, out_dir: Path, port: int,
             restore: bool = False) -> dict[str, Any]:
    case_dir = out_dir / name
    if case_dir.exists():
        shutil.rmtree(case_dir)
    case_dir.mkdir(parents=True)
    shutil.copy2(host_executable, case_dir / "EmeraldAdultHost")
    if mode != "absent":
        version_dir = case_dir / "mods" / "packages" / PLUGIN_PACKAGE / "1.0.0"
        extract_package(archive, version_dir, wrong_target=mode == "wrong_target")
        write_mod_state(case_dir, installed=True, enabled=mode != "disabled")
    else:
        write_mod_state(case_dir, installed=False)

    script = make_container_script(case_dir, workspace, config, rom, bios,
                                   state, port, restore)
    script_rel = script.relative_to(workspace).as_posix()
    mount = f"type=bind,source={workspace},target=/workspace"
    output = run([
        "docker", "run", "--rm", "--mount", mount,
        "--entrypoint", "bash", "emerald-adult-host-build", "-lc",
        f"bash /workspace/{script_rel}",
    ], cwd=workspace)
    if mode == "wrong_target" and "continuing with ROM fallback" not in output:
        raise RuntimeError(f"{name}: wrong-target selection did not fall back cleanly\n{output}")
    trace_lines = [line[len("TRACE_JSON:"):]
                   for line in output.splitlines() if line.startswith("TRACE_JSON:")]
    if len(trace_lines) != 1:
        raise RuntimeError(f"{name}: expected one host trace response\n{output}")
    observed = json.loads(trace_lines[0])
    if not observed.get("ok"):
        raise RuntimeError(f"{name}: host observer rejected status query: {observed}")
    if restore and observed.get("state_epoch", 0) <= 0:
        raise RuntimeError("savestate restore did not advance the host state epoch")
    if mode == "enabled" and state["flags"] & 3 == 3:
        if observed.get("backend") != "host_media" or not observed.get("frame_published"):
            raise RuntimeError(f"{name}: active matching media was not published: {observed}")
        if not observed.get("audio_stream_active"):
            raise RuntimeError(f"{name}: active media audio stream did not run")
        expected_cursor = (state["elapsed_vblanks"] * 65536 + 30) // 60
        if state["flags"] & 4:
            expected_cursor %= 32768
        if observed.get("audio_cursor_samples") != expected_cursor:
            raise RuntimeError(f"{name}: guest-time audio cursor differs: "
                               f"{observed.get('audio_cursor_samples')} != {expected_cursor}")
        if observed.get("audio_muted") != (not bool(state["flags"] & 8)):
            raise RuntimeError(f"{name}: guest music option did not control host audio")
        if restore and observed.get("published_frames", 0) < 2:
            raise RuntimeError(f"{name}: restored state did not publish a fresh frame")
    elif mode == "enabled":
        if observed.get("frame_published") or observed.get("audio_stream_active"):
            raise RuntimeError(f"{name}: inactive guest scene retained host output")
    elif observed.get("backend") != "gba_fallback":
        raise RuntimeError(f"{name}: expected GBA fallback: {observed}")
    if mode != "enabled" and (
            observed.get("package_loaded") or observed.get("frame_published") or
            observed.get("audio_stream_active")):
        raise RuntimeError(f"{name}: unavailable/disabled package leaked host output")
    for key in STATE_FIELDS:
        if observed.get(key) != state[key]:
            raise RuntimeError(f"{name}: {key} differs: {observed.get(key)} != {state[key]}")
    observed["name"] = name
    observed["mode"] = mode
    return observed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace-root", type=Path, required=True)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--package-meadow", type=Path, required=True)
    parser.add_argument("--package-coast", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--bios", type=Path, required=True)
    parser.add_argument("--gba-baseline", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        workspace = args.workspace_root.resolve()
        host = args.host.resolve()
        meadow = args.package_meadow.resolve()
        coast = args.package_coast.resolve()
        rom = args.rom.resolve()
        bios = args.bios.resolve()
        baseline = json.loads(args.gba_baseline.read_text(encoding="utf-8"))
        game_config = host.parent.parent / "emerald.toml"
        # Generated host build is under games/emerald_adult/<build-dir>.
        if not game_config.is_file():
            game_config = host.parent.parent.parent / "emerald.toml"
        identity = tomllib.loads(game_config.read_text(encoding="utf-8"))
        expected_rom = identity["identity"]["sha1"].lower()
        if digest(rom, "sha1") != expected_rom:
            raise ValueError("host ROM does not match emerald.toml identity")
        baseline_production_rom = baseline.get("production_rom_sha1")
        if baseline_production_rom is not None and (
                not isinstance(baseline_production_rom, str) or baseline_production_rom.lower() != expected_rom):
            raise ValueError("GBA baseline production ROM does not match the host ROM")
        header = host.parent.parent / "generated" / "host_scene_state_address.h"
        if not header.is_file():
            # Per-profile hosts live at local/<profile>/build, while generated
            # ABI headers remain shared at games/emerald_adult/generated.
            header = host.parent.parent.parent.parent / "generated" / "host_scene_state_address.h"
        address_match = re.search(
            r"kGbaHostSceneStateAddress\s*=\s*(0x[0-9A-Fa-f]+)",
            header.read_text(encoding="utf-8"))
        if not address_match or int(address_match.group(1), 16) != 0x03003A94:
            raise ValueError("generated scene state address differs from the test ABI")

        output = args.out.resolve()
        case_root = output.parent / f"{output.stem}-cases"
        case_root.mkdir(parents=True, exist_ok=True)
        scenes = baseline["scene_events"]
        events: dict[str, dict[str, Any]] = {}
        for index, name in enumerate(("once_active", "once_restored", "once_exit",
                                      "coast_loop_active", "coast_loop_exit")):
            state = scenes[name]
            archive = coast if name.startswith("coast_") else meadow
            events[name] = run_case(
                name=f"enabled-{name}", mode="enabled", state=state,
                archive=archive, host_executable=host, workspace=workspace,
                config=game_config, rom=rom, bios=bios, out_dir=case_root,
                port=19842 + index, restore=name == "once_restored")

        mode_matrix: dict[str, dict[str, Any]] = {}
        active = scenes["once_active"]
        for index, mode in enumerate(("disabled", "absent", "wrong_target"), start=10):
            mode_matrix[mode] = run_case(
                name=f"{mode}-once_active", mode=mode, state=active,
                archive=meadow, host_executable=host, workspace=workspace,
                config=game_config, rom=rom, bios=bios, out_dir=case_root,
                port=19842 + index)

        if any(mode_matrix[name][key] != active[key]
               for name in mode_matrix for key in STATE_FIELDS):
            raise ValueError("package mode changed the captured guest scene state")
        result = {
            "schema_version": 1,
            "host_identity": {
                "rom_sha1": expected_rom,
                "elf_sha256": digest(rom.with_suffix(".elf")),
                "map_sha256": digest(rom.with_suffix(".map")),
                "host_executable_sha256": digest(host),
                "game_config_sha256": digest(game_config),
                "host_branch_commit": run(["git", "-C", str(workspace / "gbarecomp"),
                                             "rev-parse", "HEAD"]).strip(),
                "host_worktree_dirty": bool(run([
                    "git", "-C", str(workspace / "gbarecomp"),
                    "status", "--porcelain",
                ]).strip()),
            },
            "events": events,
            "mode_matrix": mode_matrix,
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
        print(f"host media trace passed: {len(events)} scene events, "
              f"{len(mode_matrix) + 1} package modes; wrote {output}")
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile) as exc:
        print(f"host media trace failed: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
