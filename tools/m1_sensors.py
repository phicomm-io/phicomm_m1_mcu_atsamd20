#!/usr/bin/env python3
"""Read cached complete sensor frames through the M1 bridge, or decode hex offline.

No sensor configuration commands, firmware writes or raw UART passthrough are sent.
"""
import argparse
import json
import struct

from m1_debug import encode
from m1_network import Device

SENSORS = ('pm', 'hcho', 'sht', 'sht-status', 'sht-aux', 'hcho-probe', 'pm-probe')
LENGTHS = (32, 9, 6, 3, 6, 9, 8)


def crc8(data):
    crc = 255
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (0x31 if crc & 128 else 0)) & 255
    return crc


def decode(sensor, data):
    """Decode only verified layouts; keep unknown fields explicitly unnamed."""
    index = SENSORS.index(sensor)
    if len(data) != LENGTHS[index]:
        raise ValueError(f'{sensor}: expected {LENGTHS[index]} bytes')
    result = {'raw_hex': data.hex(' '), 'bytes': len(data)}
    word = lambda at: int.from_bytes(data[at:at + 2], 'big')
    if sensor == 'pm':
        if data[:4] != b'\x42\x4d\x00\x1c' or sum(data[:30]) != word(30):
            raise ValueError('invalid PM header, length or checksum')
        names = ('pm1_0', 'pm2_5', 'pm10')
        result.update({
            'layout': 'plantower-compatible; module model/extra fields need hardware verification',
            'frame_length': word(2),
            'cf1_ug_m3': dict(zip(names, struct.unpack('>3H', data[4:10]))),
            'atmospheric_ug_m3': dict(zip(names, struct.unpack('>3H', data[10:16]))),
            'particles_per_0_1_l_cumulative_gt_um': dict(zip(
                ('0.3', '0.5', '1.0', '2.5', '5.0', '10.0'), struct.unpack('>6H', data[16:28]))),
            'version_byte': data[28], 'error_byte': data[29], 'checksum': word(30),
            'checksum_valid': True, 'sensor_error': bool(data[29]),
        })
    elif sensor in ('hcho', 'hcho-probe'):
        command = 0x86 if sensor == 'hcho' else 0x78
        if data[:2] != bytes((255, command)) or sum(data[1:]) & 255:
            raise ValueError('invalid HCHO response or checksum')
        result.update({'command': command, 'checksum': data[8], 'checksum_valid': True})
        if sensor == 'hcho':
            result.update({'concentration_raw': word(2), 'unknown_bytes_4_7_hex': data[4:8].hex(' ')})
        else:
            result['unknown_bytes_2_7_hex'] = data[2:8].hex(' ')
    elif sensor in ('sht', 'sht-aux'):
        checks = [crc8(data[:2]) == data[2], crc8(data[3:5]) == data[5]]
        result.update({'words': [word(0), word(3)], 'crc_bytes': [data[2], data[5]],
                       'word_crc_valid': checks, 'checksum_valid': all(checks)})
        if sensor == 'sht':
            if not all(checks):
                raise ValueError('invalid SHT measurement CRC')
            result.update({'temperature_c_uncompensated': -45 + 175 * word(0) / 65535,
                           'humidity_pct_uncompensated': 100 * word(3) / 65535})
        else:
            result['meaning'] = 'unknown response to 0x3682; not an additional environmental measurement'
    elif sensor == 'sht-status':
        if crc8(data[:2]) != data[2]:
            raise ValueError('invalid SHT status CRC')
        status = word(0)
        result.update({'status_word': status, 'checksum_valid': True, 'status': {
            name: bool(status & (1 << bit)) for name, bit in (
                ('write_checksum_failed', 0), ('command_failed', 1), ('reset_detected', 4),
                ('temperature_alert', 10), ('humidity_alert', 11), ('heater_on', 13), ('alert_pending', 15))}})
    else:
        if data[:2] != b'\x42\x4d' or data[2:4] == b'\x00\x1c' or data[6:] != b'\x01\x74':
            raise ValueError('invalid PM probe recognition frame')
        result.update({'unknown_bytes_2_5_hex': data[2:6].hex(' '), 'checksum_valid': None,
                       'meaning': 'stock probe recognition pattern only; checksum semantics unknown'})
    return result


class ChangedFrame(RuntimeError):
    pass


class Reader:
    def __init__(self, device):
        self.device = device
        self.sequence = 0

    def request(self, op, payload=b''):
        self.sequence = (self.sequence + 1) & 255
        frame = encode(op, self.sequence, payload)
        reply = self.device.exchange(frame)
        if len(reply) != 20 or reply[:4] != frame[:4] or reply[-1] != 0x21:
            raise RuntimeError('unmatched sensor diagnostic response')
        if op == 16 and reply[4] == 5:
            raise ChangedFrame('sensor changed during chunked read')
        if reply[4]:
            raise RuntimeError(f'MCU diagnostic status {reply[4]} (unsupported, busy or invalid request)')
        return reply

    def capabilities(self):
        reply = self.request(0)
        if reply[11:14] != bytes((1, len(SENSORS), 9)):
            raise RuntimeError('MCU does not support sensor-frame schema 1; install a supporting MCU build')

    def metadata(self, index):
        reply = self.request(16, bytes((index,)))
        if reply[5] != index or reply[6] not in (0, LENGTHS[index]):
            raise RuntimeError('invalid sensor metadata')
        return {'length': reply[6], 'generation': int.from_bytes(reply[7:9], 'big'),
                'age_ms': int.from_bytes(reply[9:13], 'big'), 'flags': reply[13],
                'healthy': bool(reply[14])}

    def read(self, sensor):
        index = SENSORS.index(sensor)
        for _ in range(3):
            try:
                meta = self.metadata(index)
                if not meta['length']:
                    return {'captured': False, 'decoded': None}
                data = bytearray()
                for offset in range(0, meta['length'], 9):
                    count = min(9, meta['length'] - offset)
                    generation = meta['generation'].to_bytes(2, 'big')
                    reply = self.request(16, bytes((index, 1, offset, count)) + generation)
                    if reply[5:8] != bytes((index, offset, count)) or reply[8:10] != generation:
                        raise RuntimeError('invalid sensor data chunk')
                    data.extend(reply[10:10 + count])
                after = self.metadata(index)
                if after['generation'] != meta['generation']:
                    raise ChangedFrame('sensor changed after last chunk')
                flags = after.pop('flags')
                result = decode(sensor, data)
                return {**after, 'captured': True, 'flags': {
                    name: bool(flags & bit) for name, bit in (
                        ('checksum_checked', 2), ('sensor_error', 4), ('warming', 8), ('filtered', 16))},
                    'stale': after['age_ms'] > 6000,
                    'usable_for_measurement': (after['healthy'] and after['age_ms'] <= 6000 and
                                               bool(flags & 2) and not flags & 28) if index < 3 else None,
                    'decoded': result}
            except ChangedFrame:
                continue
        raise RuntimeError(f'{sensor}: sensor kept changing; retry the read')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sensor', choices=('all',) + SENSORS, default='all')
    parser.add_argument('--hex', help='decode a complete frame offline; requires a specific --sensor')
    parser.add_argument('--host')
    parser.add_argument('--device-id')
    args = parser.parse_args()
    try:
        if args.hex is not None:
            if args.sensor == 'all':
                raise ValueError('--hex requires a specific --sensor')
            result = decode(args.sensor, bytes.fromhex(args.hex))
        else:
            if not args.host or not args.device_id:
                raise ValueError('network reads require --host and --device-id')
            import os
            device = Device(args.host, os.environ.get('M1_WEB_PASSWORD', ''))
            device.identify(args.device_id)
            reader = Reader(device)
            reader.capabilities()
            selected = SENSORS if args.sensor == 'all' else (args.sensor,)
            result = {sensor: reader.read(sensor) for sensor in selected}
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (ValueError, RuntimeError, OSError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
