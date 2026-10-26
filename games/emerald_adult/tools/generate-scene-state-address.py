#!/usr/bin/env python3
"""Generate a guarded native address for the imported scene-state symbol."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

SYMBOL = "gGbaHostSceneState"
EXPECTED_SIZE = 32
IWRAM_START = 0x03000000
IWRAM_END = 0x03008000


def parse_scene_state_symbol(text: str) -> tuple[int, int]:
    rows = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise ValueError(f"line {number}: expected four TSV columns")
        addr, region, size, name = fields
        if name == SYMBOL:
            rows.append((int(addr, 16), region, int(size, 16)))
    if len(rows) != 1:
        raise ValueError(f"expected exactly one {SYMBOL} row, found {len(rows)}")
    address, region, size = rows[0]
    if region != "iwram" or size != EXPECTED_SIZE:
        raise ValueError(f"{SYMBOL} must be an {EXPECTED_SIZE}-byte IWRAM object; found {region}/{size}")
    if address & 3 or address < IWRAM_START or address + size > IWRAM_END:
        raise ValueError(f"{SYMBOL} address is unaligned or outside IWRAM: 0x{address:08X}")
    return address, size


def render_header(address: int, size: int, symbols_sha256: str) -> str:
    return (
        "// Generated from the matching imported_data_symbols.tsv; do not edit.\n"
        "#pragma once\n#include <cstdint>\n"
        f"inline constexpr std::uint32_t kGbaHostSceneStateAddress = 0x{address:08X}u;\n"
        f"inline constexpr std::uint32_t kGbaHostSceneStateIwramOffset = 0x{address - IWRAM_START:X}u;\n"
        f"inline constexpr std::uint32_t kGbaHostSceneStateSize = {size}u;\n"
        f'inline constexpr char kGbaHostSceneStateSymbolsSha256[] = "{symbols_sha256}";\n'
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    data = args.symbols.read_bytes()
    try:
        address, size = parse_scene_state_symbol(data.decode("utf-8"))
    except (UnicodeError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
    digest = hashlib.sha256(data).hexdigest()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render_header(address, size, digest), encoding="utf-8", newline="\n")
    print(f"generated {args.out} for {SYMBOL} at 0x{address:08X}, TSV SHA-256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
