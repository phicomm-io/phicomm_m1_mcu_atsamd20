#!/usr/bin/env python3
"""Encode MCU debug frames offline. Does not open a UART or send over the network."""
import argparse
import struct


def encode(op, seq, payload=b''):
    if len(payload) > 7 or not 0 <= seq <= 255:
        raise ValueError('payload <=7 bytes and sequence 0..255 required')
    return bytes((0x23, 0x70, op, seq)) + payload.ljust(7, b'\0') + b'\x21'


def encode_threshold(sensor, low, high):
    if sensor not in ('hcho', 'pm25') or not 0 <= low <= high <= 65535:
        raise ValueError('thresholds require hcho/pm25 and 0 <= low <= high <= 65535')
    return bytes((0x23, 5 if sensor == 'hcho' else 6)) + struct.pack('>HH', low, high) + bytes(5) + b'\x21'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seq', type=int, default=1)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('caps', 'status', 'exit', 'thresholds', 'alarms'):
        sub.add_parser(name)
    p = sub.add_parser('tone')
    p.add_argument('name', choices=('off', 'tap', 'confirm', 'notice', 'error'))
    p = sub.add_parser('pwm')
    p.add_argument('hz', type=int); p.add_argument('duty', type=int); p.add_argument('milliseconds', type=int)
    p = sub.add_parser('sample')
    p.add_argument('hcho', type=int); p.add_argument('pm', type=int)
    p.add_argument('temperature_centi', type=int); p.add_argument('humidity_percent', type=int)
    p = sub.add_parser('display')
    p.add_argument('brightness', type=int); p.add_argument('on', type=int, choices=(0, 1))
    p.add_argument('all_segments', type=int, choices=(0, 1)); p.add_argument('milliseconds', type=int)
    p = sub.add_parser('raw')
    p.add_argument('offset', type=lambda s: int(s, 0)); p.add_argument('data', help='1..5 bytes in hex')
    p = sub.add_parser('read')
    p.add_argument('offset', type=lambda s: int(s, 0)); p.add_argument('count', type=int)
    p = sub.add_parser('pm-mode')
    p.add_argument('mode', type=int, choices=(0, 2, 3))
    p = sub.add_parser('threshold')
    p.add_argument('sensor', choices=('hcho', 'pm25'))
    p.add_argument('low', type=int); p.add_argument('high', type=int)
    p = sub.add_parser('alarm-mask', help='0=off; bits 0 HCHO, 1 PM2.5, 2 legacy CO2')
    p.add_argument('mask', type=int, choices=range(8))
    a = parser.parse_args()
    payload = b''
    operations = dict(caps=0, pwm=1, sample=2, display=3, raw=4, exit=5, status=6, **{'pm-mode': 7, 'alarm-mask': 20}, read=8, thresholds=9, tone=14, alarms=20)
    try:
        if a.command == 'threshold':
            print(encode_threshold(a.sensor, a.low, a.high).hex(' '))
            return
        if a.command == 'tone':
            payload = bytes((('off', 'tap', 'confirm', 'notice', 'error').index(a.name),))
        elif a.command == 'pwm':
            if not (0 <= a.duty <= 100 and (a.duty == 0 or (100 <= a.hz <= 10000 and 1 <= a.milliseconds <= 10000))):
                raise ValueError('PWM requires 100..10000 Hz, duty 0..100 and 1..10000 ms; duty 0 stops')
            payload = struct.pack('>HBH', a.hz, a.duty, a.milliseconds)
        elif a.command == 'sample':
            if not (0 <= a.hcho <= 9999 and 0 <= a.pm <= 999 and -4000 <= a.temperature_centi <= 12500 and 0 <= a.humidity_percent <= 100):
                raise ValueError('sample outside supported display range')
            payload = struct.pack('>HHhB', a.hcho, a.pm, a.temperature_centi, a.humidity_percent)
        elif a.command == 'display':
            if a.brightness not in (0, 25, 50, 75, 100) or not 1 <= a.milliseconds <= 10000:
                raise ValueError('invalid brightness or lease')
            payload = struct.pack('>BBBH', a.brightness, a.on, a.all_segments, a.milliseconds)
        elif a.command == 'raw':
            data = bytes.fromhex(a.data)
            if not (1 <= len(data) <= 5 and 0 <= a.offset < 29 and a.offset + len(data) <= 29):
                raise ValueError('RAM write outside 29 bytes or chunk size 1..5')
            payload = bytes((a.offset, len(data))) + data
        elif a.command == 'read':
            if not (1 <= a.count <= 12 and 0 <= a.offset < 29 and a.offset + a.count <= 29):
                raise ValueError('RAM read outside 29 bytes or count 1..12')
            payload = bytes((a.offset, a.count))
        elif a.command == 'pm-mode':
            payload = bytes((a.mode,))
        elif a.command == 'alarm-mask':
            payload = bytes((1, a.mask))
        print(encode(operations[a.command], a.seq, payload).hex(' '))
    except (ValueError, struct.error) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
