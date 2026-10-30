#!/usr/bin/env python3
"""Package M5-5 generated frame/audio outputs as a bounded data-only .gbamod."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tomllib
import zipfile
from pathlib import Path, PurePosixPath

PLUGIN_ID = "pokemon-adult.gallery-media-seam.v1"
MAX_FILES = 4096
MAX_EXPANDED = 256 * 1024 * 1024


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def package(source: Path, output: Path, license_text: Path, version: str,
            license_id: str, config_path: Path | None = None) -> None:
    source = source.resolve()
    output = output.resolve()
    license_text = license_text.resolve()
    manifest_path = source / "manifest.json"
    if not manifest_path.is_file() or not license_text.is_file():
        raise ValueError("input directory needs manifest.json and --license-file must exist")
    metadata = json.loads(manifest_path.read_text(encoding="utf-8"))
    if metadata.get("schema_version") != 1:
        raise ValueError("unsupported host media manifest schema")
    profile = metadata.get("source", {}).get("frame_size_profile")
    profile_sizes = {"native_3_2": (240, 160), "landscape_16_9": (160, 90),
                     "portrait_9_16": (90, 160)}
    if (profile not in profile_sizes
            or (metadata.get("source", {}).get("source_width"),
                metadata.get("source", {}).get("source_height")) != profile_sizes[profile]):
        raise ValueError("unsupported or inconsistent source video profile")
    frames_meta = metadata.get("frames", {})
    audio_meta = metadata.get("audio", {})
    timeline = metadata.get("timeline", {})
    if (frames_meta.get("width"), frames_meta.get("height"), frames_meta.get("format"),
            frames_meta.get("frame_bytes")) != (240, 160, "BGR555LE", 76800):
        raise ValueError("host frames must use the supported 240x160 BGR555LE format")
    if (audio_meta.get("format") != "signed 16-bit little-endian mono PCM"
            or audio_meta.get("sample_rate") != 65536 or audio_meta.get("channels") != 1
            or audio_meta.get("sample_count", 0) * 2 != audio_meta.get("expanded_bytes")
            or timeline.get("guest_clock_hz") != 60
            or timeline.get("sample_rate") != 65536
            or timeline.get("total_samples") != audio_meta.get("sample_count")):
        raise ValueError("host audio metadata is inconsistent with the guest timeline")
    timeline_frames = timeline.get("frames")
    total_vblanks = timeline.get("total_vblanks")
    if (not isinstance(timeline_frames, list) or not timeline_frames
            or any(frame.get("index") != index for index, frame in enumerate(timeline_frames))
            or type(total_vblanks) is not int or total_vblanks <= 0
            or timeline.get("total_samples") != (total_vblanks * 65536 + 30) // 60
            or sum(frame.get("duration_vblanks", 0) for frame in timeline_frames) != total_vblanks):
        raise ValueError("host video timeline is incomplete or inconsistent")
    files = sorted(path for path in source.iterdir() if path.is_file())
    if manifest_path not in files or any(path.is_symlink() for path in files):
        raise ValueError("media inputs must be regular, direct child files")
    entries: list[tuple[str, bytes]] = []
    for path in files:
        name = path.name
        if name in {"manifest.toml", "LICENSE.txt"} or PurePosixPath(name).name != name:
            raise ValueError(f"unsafe or reserved package path: {name}")
        entries.append((name, path.read_bytes()))
    declared = {item["file"] for item in frames_meta["chunks"]}
    declared.add(metadata["audio"]["file"])
    expected = declared | {"manifest.json"}
    if {name for name, _ in entries} != expected:
        raise ValueError("media directory files do not exactly match manifest.json")
    frame_cursor = 0
    for chunk in frames_meta["chunks"]:
        if (chunk.get("first_frame") != frame_cursor
                or chunk.get("frame_count", 0) <= 0
                or chunk.get("expanded_bytes") != chunk.get("frame_count") * 76800):
            raise ValueError("frame chunk ranges or expanded sizes are inconsistent")
        data = dict(entries)[chunk["file"]]
        if len(data) != chunk["expanded_bytes"] or hashlib.sha256(data).hexdigest() != chunk["sha256"]:
            raise ValueError(f"frame chunk does not match host manifest: {chunk['file']}")
        frame_cursor += chunk["frame_count"]
    if frame_cursor != len(timeline_frames):
        raise ValueError("frame chunk count does not match the guest timeline")
    audio_data = dict(entries)[audio_meta["file"]]
    if (len(audio_data) != audio_meta["expanded_bytes"]
            or hashlib.sha256(audio_data).hexdigest() != audio_meta["sha256"]):
        raise ValueError("audio chunk does not match host manifest")
    license_bytes = license_text.read_bytes()
    if not license_bytes:
        raise ValueError("license file is empty")
    entries.append(("LICENSE.txt", license_bytes))
    if len(entries) + 1 > MAX_FILES or sum(len(data) for _, data in entries) > MAX_EXPANDED:
        raise ValueError("package exceeds runtime entry or expanded-size limits")
    if not license_id:
        raise ValueError("license identifier must not be empty")
    config_path = config_path or Path(__file__).resolve().parent.parent / "emerald.toml"
    config = tomllib.loads(config_path.read_text(encoding="utf-8"))
    game_id = config["program"]["id"]
    rom_sha1 = config["identity"]["sha1"]
    if not re.fullmatch(r"[0-9a-f]{40}", rom_sha1) or config.get("rom", {}).get("sha1") != rom_sha1:
        raise ValueError("emerald.toml does not contain a valid pinned ROM SHA-1")

    lines = [
        "format_version = 1", 'id = "adult.gallery-media"', f"version = {quote(version)}",
        'name = "Adult Gallery Media (neutral sample)"', 'author = "Pokemon Projects"',
        'description = "Neutral host frame and audio data generated by the pinned M5-5 pipeline."',
        f'license = {quote(license_id)}', 'resolver = "declarative"',
        'save_compatibility = "shared"', "", "[[target]]",
        f"game_id = {quote(game_id)}", f"rom_sha1 = {quote(rom_sha1)}", "",
        "[[feature]]", 'id = "gallery-media"', 'name = "Gallery host media"',
        'description = "Optional video frames and synchronized audio for the native host."',
        'group = "Presentation"', "default_enabled = false", "", "[[plugin]]",
        'feature = "gallery-media"', f"id = {quote(PLUGIN_ID)}", "",
    ]
    all_entries = entries + [("manifest.toml", b"")]
    package_asset_ids: set[str] = set()
    for name, data in all_entries:
        if name == "manifest.toml":
            continue
        asset_id = "media-" + re.sub(r"[^a-z0-9-]+", "-", name.lower()).strip("-")
        if asset_id in package_asset_ids:
            raise ValueError("media asset names produce duplicate package IDs")
        package_asset_ids.add(asset_id)
        lines.extend([
            "[[package_asset]]", 'feature = "gallery-media"',
            f"id = {quote(asset_id)}",
            f"path = {quote(name)}", f"sha1 = {quote(sha1(data))}",
            f"size = {len(data)}", f"purpose = {quote('Packaged host media and provenance: ' + name)}", "",
        ])
    manifest = ("\n".join(lines)).encode("utf-8")
    archive_entries = entries + [("manifest.toml", manifest)]
    if len(archive_entries) > MAX_FILES or sum(len(data) for _, data in archive_entries) > MAX_EXPANDED:
        raise ValueError("final package exceeds runtime entry or expanded-size limits")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(archive_entries):
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media_dir", type=Path, help="M5-5 host media output directory")
    parser.add_argument("output", type=Path, help="output .gbamod archive")
    parser.add_argument("--license-file", required=True, type=Path,
                        help="license text for all packaged derived outputs")
    parser.add_argument("--license-id", required=True,
                        help="license identifier recorded in manifest.toml")
    parser.add_argument("--version", default="1.0.0", help="package version (default: 1.0.0)")
    parser.add_argument("--config", type=Path, help="explicit matching ROM config (default: production emerald.toml)")
    args = parser.parse_args()
    if args.output.suffix != ".gbamod":
        parser.error("output must end in .gbamod")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("--version must use numeric major.minor.patch form")
    try:
        package(args.media_dir, args.output, args.license_file, args.version,
                args.license_id, args.config)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, json.JSONDecodeError,
            zipfile.BadZipFile) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
