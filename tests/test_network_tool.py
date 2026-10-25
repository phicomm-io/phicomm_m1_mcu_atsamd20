import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import m1_network as net


class NetworkToolTest(unittest.TestCase):
    def test_manifest_and_vector(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'app.bin'
            data = struct.pack('<II', 0x20008000, 0x4041) + bytes(256)
            image.write_bytes(data)
            manifest = image.with_suffix('.json')
            meta = dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data),
                        artifact=image.name, target='ATSAMD20G18A', link_address='0x4000',
                        ota_protocol='m1-uart-0x12-0x16', uart_version='test')
            manifest.write_text(json.dumps(meta))
            self.assertEqual(net.load_image(image, manifest)[0], data)
            image.write_bytes(data + b'x')
            with self.assertRaises(ValueError):
                net.load_image(image, manifest)

    def test_paced_transfer_and_runtime_proof(self):
        class Fake:
            def __init__(self):
                self.sent = []
            def identify(self, identity):
                return dict(uart_ready=True, version='wifi')
            def command(self, action):
                return dict(version_received=True, version='new' if len(self.sent) == 4 else 'old')
            def exchange(self, frame):
                self.sent.append(frame)
                if frame[1] == 0x12:
                    return bytes((0x23, 0x14)) + bytes(17) + b'\x21'
                if frame[1] == 0x13:
                    return b'\x23\x14' + frame[2:4] + b'\x01' + bytes(14) + b'\x21'
                return b'\x23\x16' + bytes(17) + b'\x21'
        fake = Fake()
        data = struct.pack('<II', 0x20008000, 0x4041) + bytes(256)
        with patch.object(net.time, 'sleep'):
            result = net.flash(fake, 'aabbccddeeff', data, 'new')
        self.assertEqual(result['version'], 'new')
        self.assertEqual([p[1] for p in fake.sent], [0x12, 0x13, 0x13, 0x16])
        broken = bytearray(fake.exchange(fake.sent[1])); broken[3] ^= 1
        with self.assertRaises(RuntimeError):
            net.validate_ack(fake.sent[1], broken)

    def test_same_version_waits_for_fresh_runtime(self):
        class Fake:
            def __init__(self):
                self.polls = 0
            def identify(self, identity):
                return dict(uart_ready=True, version='wifi')
            def command(self, action):
                self.polls += 1
                # Preflight, still upgrading, timed-out stale cache, reboot.
                return dict(version_received=True, version='same',
                            updating=self.polls == 2,
                            version_age_ms=100000 if self.polls < 4 else 0)
            def exchange(self, frame):
                if frame[1] == 0x12:
                    return bytes((0x23, 0x14)) + bytes(17) + b'\x21'
                if frame[1] == 0x13:
                    return b'\x23\x14' + frame[2:4] + b'\x01' + bytes(14) + b'\x21'
                return b'\x23\x16' + bytes(17) + b'\x21'
        fake = Fake()
        data = struct.pack('<II', 0x20008000, 0x4041) + bytes(256)
        with patch.object(net.time, 'sleep'):
            net.flash(fake, 'aabbccddeeff', data, 'same')
        self.assertEqual(fake.polls, 4)


if __name__ == '__main__':
    unittest.main()
