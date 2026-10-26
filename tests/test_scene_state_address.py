from __future__ import annotations

import hashlib
import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).parent.parent / "games/emerald_adult/tools/generate-scene-state-address.py"
SPEC = importlib.util.spec_from_file_location("scene_state_address", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


class SceneStateAddressTests(unittest.TestCase):
    def test_parses_and_renders_matching_import(self) -> None:
        tsv = "# addr\tregion\tsize\tname\n0x03000100\tiwram\t0x20\tgGbaHostSceneState\n"
        address, size = MODULE.parse_scene_state_symbol(tsv)
        output = MODULE.render_header(address, size, hashlib.sha256(tsv.encode()).hexdigest())
        self.assertEqual(address, 0x03000100)
        self.assertEqual(size, 32)
        self.assertIn("kGbaHostSceneStateAddress = 0x03000100u", output)
        self.assertIn("kGbaHostSceneStateIwramOffset = 0x100u", output)
        self.assertIn(hashlib.sha256(tsv.encode()).hexdigest(), output)

    def test_rejects_bad_or_ambiguous_import(self) -> None:
        cases = (
            "0x03000100\tiwram\t0x1C\tgGbaHostSceneState\n",
            "0x02000100\tewram\t0x20\tgGbaHostSceneState\n",
            "0x03000100\tiwram\t0x20\tgGbaHostSceneState\n"
            "0x03000200\tiwram\t0x20\tgGbaHostSceneState\n",
            "0x03000102\tiwram\t0x20\tgGbaHostSceneState\n",
            "0x03000100\tiwram\t0x20\tdifferent_symbol\n",
        )
        for tsv in cases:
            with self.subTest(tsv=tsv), self.assertRaises(ValueError):
                MODULE.parse_scene_state_symbol(tsv)


if __name__ == "__main__":
    unittest.main()
