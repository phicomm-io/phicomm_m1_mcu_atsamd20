import subprocess
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ToneToolTests(unittest.TestCase):
    def invoke(self, *arguments):
        return subprocess.run([sys.executable, str(ROOT/'tools/m1_debug.py'), *arguments],
                              capture_output=True, text=True)

    def test_preset_frames(self):
        for ident, name in enumerate(('off', 'tap', 'confirm', 'notice', 'error')):
            with self.subTest(name=name):
                result = self.invoke('--seq', '35', 'tone', name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(bytes.fromhex(result.stdout),
                                 bytes((0x23, 0x70, 14, 35, ident)) + bytes(6) + b'\x21')

    def test_invalid_presets_and_sequence_produce_no_frame(self):
        for args in [('tone', 'unknown'), ('tone', '-1'), ('--seq', '256', 'tone', 'tap')]:
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')
