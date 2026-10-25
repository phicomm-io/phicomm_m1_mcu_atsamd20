from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from m1_sensors import Reader, decode, crc8, LENGTHS


def pm_frame(error=0):
    data = bytearray.fromhex('42 4d 00 1c')
    for value in range(257, 257 * 13, 257):
        data.extend(value.to_bytes(2, 'big'))
    data.extend((17, error))
    data.extend(sum(data).to_bytes(2, 'big'))
    return bytes(data)


class Fake:
    def __init__(self, changes=0):
        self.generation = 1
        self.changes = changes
        self.data = pm_frame()
        self.frames = []
        self.empty = False
        self.bad_reply = False
        self.flags = 3
        self.age = 12

    def exchange(self, frame):
        self.frames.append(frame)
        reply = bytearray(20)
        reply[:4] = frame[:4]
        reply[-1] = 0x21
        if frame[2] == 0:
            reply[5] = 2
            reply[11:14] = bytes((1, len(LENGTHS), 9))
        else:
            sensor, mode, offset, count = frame[4:8]
            reply[5] = sensor
            if mode == 0:
                reply[6] = 0 if self.empty else len(self.data)
                reply[7:9] = self.generation.to_bytes(2, 'big')
                reply[9:13] = self.age.to_bytes(4, 'big')
                reply[13:15] = bytes((self.flags, 1))
            else:
                if self.changes:
                    self.changes -= 1
                    self.generation += 1
                if int.from_bytes(frame[8:10], 'big') != self.generation:
                    reply[4] = 5
                else:
                    reply[6:8] = bytes((offset, count))
                    reply[8:10] = frame[8:10]
                    reply[10:10 + count] = self.data[offset:offset + count]
        if self.bad_reply:
            reply[3] ^= 1
        return bytes(reply)


class SensorToolTests(unittest.TestCase):
    def test_pm_all_fields_and_error_frame(self):
        result = decode('pm', pm_frame(2))
        self.assertEqual(list(result['cf1_ug_m3'].values()), [257, 514, 771])
        self.assertEqual(list(result['atmospheric_ug_m3'].values()), [1028, 1285, 1542])
        self.assertEqual(list(result['particles_per_0_1_l_cumulative_gt_um'].values()),
                         [1799, 2056, 2313, 2570, 2827, 3084])
        self.assertTrue(result['sensor_error'])
        self.assertEqual(result['version_byte'], 17)
        data = bytearray(pm_frame()); data[8] ^= 1
        with self.assertRaises(ValueError): decode('pm', data)
        with self.assertRaises(ValueError): decode('pm', data[:31])

    def test_hcho_unknown_fields_preserved(self):
        data = bytearray.fromhex('ff 86 01 02 11 22 33 44')
        data.append((-sum(data[1:])) & 255)
        result = decode('hcho', data)
        self.assertEqual(result['concentration_raw'], 258)
        self.assertEqual(result['unknown_bytes_4_7_hex'], '11 22 33 44')
        data[-1] ^= 1
        with self.assertRaises(ValueError): decode('hcho', data)

    def test_sht_crc_status_and_unknown_aux(self):
        data = bytes((0x66, 0x66, crc8(b'\x66\x66'), 0x80, 0, crc8(b'\x80\x00')))
        result = decode('sht', data)
        self.assertAlmostEqual(result['temperature_c_uncompensated'], 25)
        self.assertAlmostEqual(result['humidity_pct_uncompensated'], 50.00076, places=4)
        bad = data[:-1] + bytes((data[-1] ^ 1,))
        with self.assertRaises(ValueError): decode('sht', bad)
        self.assertFalse(decode('sht-aux', bad)['checksum_valid'])
        status = bytes.fromhex('ac 13')
        result = decode('sht-status', status + bytes((crc8(status),)))
        self.assertTrue(all(result['status'].values()))

    def test_reader_coherent_retry_and_read_only(self):
        fake = Fake(changes=1)
        reader = Reader(fake); reader.capabilities()
        result = reader.read('pm')
        self.assertEqual(result['generation'], 2)
        self.assertEqual(result['decoded']['raw_hex'], fake.data.hex(' '))
        self.assertTrue(result['usable_for_measurement'])
        self.assertTrue(all(f[1:3] in (b'\x70\x00', b'\x70\x10') for f in fake.frames))
        fake.flags = 11
        self.assertFalse(reader.read('pm')['usable_for_measurement'])
        fake.flags = 3; fake.age = 6001
        self.assertTrue(reader.read('pm')['stale'])
        self.assertFalse(reader.read('pm')['usable_for_measurement'])

    def test_reader_missing_changing_and_unmatched(self):
        fake = Fake(); fake.empty = True
        self.assertEqual(Reader(fake).read('pm'), {'captured': False, 'decoded': None})
        with self.assertRaisesRegex(RuntimeError, 'kept changing'):
            Reader(Fake(changes=3)).read('pm')
        fake = Fake(); fake.bad_reply = True
        with self.assertRaisesRegex(RuntimeError, 'unmatched'):
            Reader(fake).capabilities()


if __name__ == '__main__':
    unittest.main()
