"""Host checks for the optional .cpfont content revision in the update manifest."""

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


converter = load_module("fontconvert_sdcard", ROOT / "lib/EpdFont/scripts/fontconvert_sdcard.py")
manifest = load_module("generate_font_manifest", ROOT / "scripts/generate-font-manifest.py")


class FontContentRevisionTest(unittest.TestCase):
    def test_same_size_glyph_edit_changes_revision_and_manifest(self):
        original = converter.compute_content_revision({0: [b"glyph advance=467"]})
        changed = converter.compute_content_revision({0: [b"glyph advance=627"]})
        self.assertNotEqual(original, changed)
        with tempfile.TemporaryDirectory() as directory:
            font = Path(directory) / "Example_14.cpfont"
            font.write_bytes(struct.pack("<8sHHBI15s", b"CPFONT\0\0", 5, 1, 1, changed, bytes(15)) + bytes(32))
            self.assertEqual(manifest.read_cpfont_content_revision(font), changed)
            entry = manifest.build_manifest({"Example": [font]}, "https://example.invalid/")["families"][0]["files"][0]
            self.assertEqual(entry["contentRevision"], changed)

    def test_legacy_header_falls_back_to_size_only(self):
        with tempfile.TemporaryDirectory() as directory:
            font = Path(directory) / "Example_14.cpfont"
            font.write_bytes(struct.pack("<8sHHB19s", b"CPFONT\0\0", 5, 1, 1, bytes(19)) + bytes(32))
            entry = manifest.build_manifest({"Example": [font]}, "https://example.invalid/")["families"][0]["files"][0]
            self.assertNotIn("contentRevision", entry)


if __name__ == "__main__":
    unittest.main()
