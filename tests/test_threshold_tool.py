import subprocess
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ThresholdToolTests(unittest.TestCase):
    def invoke(self, *arguments):
        return subprocess.run([sys.executable, str(ROOT/'tools/m1_debug.py'), *arguments],
                              capture_output=True, text=True)

    def test_original_wire_format_and_extended_readback(self):
        cases = [(['threshold', 'hcho', '100', '300'], '23 05 00 64 01 2c 00 00 00 00 00 21'),
                 (['threshold', 'pm25', '4660', '43981'], '23 06 12 34 ab cd 00 00 00 00 00 21'),
                 (['threshold', 'pm25', '0', '0'], '23 06 00 00 00 00 00 00 00 00 00 21'),
                 (['threshold', 'hcho', '65535', '65535'], '23 05 ff ff ff ff 00 00 00 00 00 21'),
                 (['--seq', '35', 'thresholds'], '23 70 09 23 00 00 00 00 00 00 00 21')]
        for args, expected in cases:
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_bad_values_produce_no_frame(self):
        for low, high in [('-1', '100'), ('0', '65536'), ('200', '100'), ('1.5', '100')]:
            with self.subTest(low=low, high=high):
                result = self.invoke('threshold', 'hcho', low, high)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')

    def test_optional_buzzer_configuration_frames(self):
        for arguments, expected in [
            (('alarms',), '23 70 14 01 00 00 00 00 00 00 00 21'),
            (('alarm-mask', '0'), '23 70 14 01 01 00 00 00 00 00 00 21'),
            (('--seq', '255', 'alarm-mask', '3'), '23 70 14 ff 01 03 00 00 00 00 00 21'),
        ]:
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
        for mask in ('-1', '8', 'true', '1.5'):
            result = self.invoke('alarm-mask', mask)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
