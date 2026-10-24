import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("analyze_stock", ROOT / "tools/analyze_stock.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


class AnalyzeTests(unittest.TestCase):
    def test_unknown_image_does_not_receive_snapshot_labels(self):
        b = bytearray(b"\xff" * 0x40000)
        struct.pack_into("<II", b, 0, 0x20008000, 0x829)
        struct.pack_into("<II", b, 0x4000, 0x20004000, 0x4101)
        r = analyzer.analyze(b)
        self.assertFalse(r["known_snapshot"])
        self.assertNotIn("tasks", r)
        self.assertTrue(all(v["plausible"] for v in r["vectors"]))
        self.assertEqual(r["non_ff_blocks_4k"], ["0x0", "0x4000"])

    def test_truncated_dump_rejected(self):
        with self.assertRaises(ValueError):
            analyzer.analyze(bytes(128 * 1024))

    def test_erased_dump_has_no_vector(self):
        r = analyzer.analyze(b"\xff" * 0x40000)
        self.assertIsNone(r["last_non_ff"])
        self.assertFalse(any(v["plausible"] for v in r["vectors"]))
