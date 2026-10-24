import importlib.util
import json
import hashlib
import ctypes
import subprocess
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pack_ota', ROOT / 'tools/pack_ota.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


def image(n):
    return struct.pack('<II', 0x20008000, 0x4041) + bytes((i * 13 & 255 for i in range(n - 8)))


class OTATests(unittest.TestCase):
    def test_vectors_and_boundaries(self):
        for n in (164, 255, 256, 512, 65535):
            data = image(n)
            self.assertEqual(ota.validate(data), n)
            packets = list(ota.frames(data))
            self.assertEqual(packets[0], bytes((0x23, 0x12, n // 256, n % 256)) + bytes(7) + b'\x21')
            self.assertEqual(len(packets), n // 256 + 3)
            self.assertEqual(b''.join(p[4:-2] for p in packets[1:-1]), data)
            self.assertEqual(len(packets[-2]), n % 256 + 6)
        for n in (163, 65536):
            with self.assertRaises(ValueError):
                ota.validate(image(n))
        for sp, pc in ((0x20008004, 0x4041), (0x20008000, 0x41), (0x20008000, 0x4040), (0x20008000, 0x4fff)):
            with self.assertRaises(ValueError):
                ota.validate(struct.pack('<II', sp, pc) + bytes(200))

    def test_packet_crc_matches_stock_verified_c_receiver(self):
        with tempfile.TemporaryDirectory() as d:
            libpath = Path(d) / 'core.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-I', str(ROOT / 'include'),
                            str(ROOT / 'src/core.c'), '-o', str(libpath)], check=True)
            lib = ctypes.CDLL(str(libpath))
            lib.m1_stock_crc_byte.argtypes = [ctypes.c_uint16, ctypes.c_uint8]
            lib.m1_stock_crc_byte.restype = ctypes.c_uint16
            for packet in list(ota.frames(image(777)))[1:-1]:
                crc = 0
                for byte in packet[4:-2]:
                    crc = lib.m1_stock_crc_byte(crc, byte)
                self.assertEqual(struct.unpack('>H', packet[-2:])[0], crc)
            self.assertEqual(ota.crc_block(b''), 0)

    def test_package_preserves_exact_raw_payload_and_provenance(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            app = root / 'm1-app.bin'
            data = image(512)
            app.write_bytes(data)
            meta = root / 'app.json'
            record = dict(artifact=app.name, sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), source_commit='test', source_dirty=True)
            meta.write_text(json.dumps(record))
            result = ota.package(app, meta, root / 'ota')
            self.assertEqual((root / 'ota/m1-mcu-ota.bin').read_bytes(), data)
            self.assertFalse(result['image_signature'])
            self.assertFalse(result['release'])
            self.assertTrue(result['source_dirty'])
            self.assertEqual(result['data_frames'], 3)
            for line in (root / 'ota/SHA256SUMS').read_text().splitlines():
                sha, name = line.split()
                self.assertEqual(hashlib.sha256((root / 'ota' / name).read_bytes()).hexdigest(), sha)
            record['sha256'] = '0' * 64
            meta.write_text(json.dumps(record))
            with self.assertRaises(ValueError):
                ota.package(app, meta, root / 'bad')
