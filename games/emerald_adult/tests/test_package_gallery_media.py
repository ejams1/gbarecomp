#!/usr/bin/env python3
"""Tests ROM identity selection and validation in the media packager."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import tomllib
import unittest
import zipfile
from pathlib import Path


TOOL = Path(__file__).resolve().parents[1] / "tools" / "package_gallery_media.py"
SPEC = importlib.util.spec_from_file_location("package_gallery_media", TOOL)
assert SPEC is not None and SPEC.loader is not None
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


class GalleryMediaPackagerIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.media = self.root / "media"
        self.media.mkdir()
        self.license = self.root / "LICENSE.txt"
        self.license.write_text("Neutral test media license\n", encoding="utf-8")
        frame = bytes(240 * 160 * 2)
        audio = bytes(1092 * 2)
        (self.media / "frames_000.bgr555le").write_bytes(frame)
        (self.media / "audio.s16le").write_bytes(audio)
        manifest = {
            "schema_version": 1,
            "source": {
                "source_width": 240,
                "source_height": 160,
                "frame_size_profile": "native_3_2",
            },
            "frames": {
                "width": 240,
                "height": 160,
                "format": "BGR555LE",
                "frame_bytes": len(frame),
                "chunks": [{
                    "file": "frames_000.bgr555le",
                    "first_frame": 0,
                    "frame_count": 1,
                    "expanded_bytes": len(frame),
                    "sha256": hashlib.sha256(frame).hexdigest(),
                }],
            },
            "audio": {
                "format": "signed 16-bit little-endian mono PCM",
                "sample_rate": 65536,
                "channels": 1,
                "sample_count": 1092,
                "file": "audio.s16le",
                "expanded_bytes": len(audio),
                "sha256": hashlib.sha256(audio).hexdigest(),
            },
            "timeline": {
                "guest_clock_hz": 60,
                "loop_policy": "ONCE",
                "sample_rate": 65536,
                "total_vblanks": 1,
                "total_samples": 1092,
                "frames": [{
                    "index": 0,
                    "media_id": "SCENE_MEDIA_ID_MEADOW_0",
                    "duration_vblanks": 1,
                    "start_vblank": 0,
                    "end_vblank": 1,
                    "start_sample": 0,
                    "end_sample": 1092,
                }],
            },
        }
        (self.media / "manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8")
        self.config = self.root / "agent-test.toml"
        self.config.write_text(
            '[program]\nid = "pokemon-modern-emerald"\n'
            '[identity]\nsha1 = "' + "a" * 40 + '"\n'
            '[rom]\nsha1 = "' + "a" * 40 + '"\n',
            encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def package_to(self, output: Path, config: Path | None = None) -> None:
        PACKAGER.package(self.media, output, self.license, "1.2.3",
                         "CC0-1.0", config)

    def test_explicit_config_sets_package_rom_target(self) -> None:
        output = self.root / "agent-test.gbamod"
        subprocess.run([
            sys.executable,
            str(TOOL),
            str(self.media),
            str(output),
            "--license-file", str(self.license),
            "--license-id", "CC0-1.0",
            "--config", str(self.config),
        ], check=True, capture_output=True, text=True)
        with zipfile.ZipFile(output) as archive:
            manifest = tomllib.loads(archive.read("manifest.toml").decode("utf-8"))
        self.assertEqual(manifest["target"], [{
            "game_id": "pokemon-modern-emerald",
            "rom_sha1": "a" * 40,
        }])

    def test_default_config_remains_the_production_target(self) -> None:
        production_config = TOOL.parent.parent / "emerald.toml"
        expected = tomllib.loads(production_config.read_text(encoding="utf-8"))
        output = self.root / "production.gbamod"
        self.package_to(output)
        with zipfile.ZipFile(output) as archive:
            manifest = tomllib.loads(archive.read("manifest.toml").decode("utf-8"))
        self.assertEqual(manifest["target"], [{
            "game_id": expected["program"]["id"],
            "rom_sha1": expected["identity"]["sha1"],
        }])

    def test_config_with_stale_rom_identity_is_rejected_without_output(self) -> None:
        config_text = self.config.read_text(encoding="utf-8")
        config_text = config_text.replace('sha1 = "' + "a" * 40 + '"',
                                          'sha1 = "' + "b" * 40 + '"', 1)
        self.config.write_text(config_text, encoding="utf-8")
        output = self.root / "wrong-target.gbamod"
        with self.assertRaisesRegex(ValueError, "valid pinned ROM SHA-1"):
            self.package_to(output, self.config)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
