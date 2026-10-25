#!/usr/bin/env python3
"""Paced MCU OTA through the M1 Wi-Fi firmware's authenticated UART bridge."""
import argparse
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import time

from pack_ota import frames, validate


class Device:
    def __init__(self, host, password=''):
        self.host = host
        self.headers = {}
        if password:
            token = base64.b64encode(('admin:' + password).encode()).decode()
            self.headers['Authorization'] = 'Basic ' + token

    def request(self, path, body=None):
        conn = http.client.HTTPConnection(self.host, timeout=8)
        try:
            headers = dict(self.headers)
            if body is not None:
                headers['Content-Type'] = 'application/octet-stream'
            conn.request('GET' if body is None else 'POST', path, body, headers)
            reply = conn.getresponse()
            data = reply.read()
            if reply.status != 200:
                raise RuntimeError(f'HTTP {reply.status}: {data[:200]!r}')
            return data
        finally:
            conn.close()

    def command(self, action):
        raw = self.request('/api/v1/command', json.dumps({'action': action, 'params': {}}).encode())
        result = json.loads(raw)
        if result.get('code') != 'ok':
            raise RuntimeError(f'{action}: {result.get("code")}')
        return result['data']

    def identify(self, device_id):
        status = self.command('status.get')
        capabilities = self.command('capabilities.get')
        if capabilities.get('model') != 'm1' or status.get('device_id', '').lower() != device_id.lower():
            raise RuntimeError('device identity mismatch')
        return status

    def exchange(self, frame):
        response = self.request('/api/v1/mcu/exchange', frame)
        if len(response) != 20 or response[0] != 0x23 or response[-1] != 0x21:
            raise RuntimeError('invalid bridge response')
        return response


def validate_ack(frame, reply):
    if len(reply) != 20 or reply[0] != 0x23 or reply[-1] != 0x21:
        raise RuntimeError('invalid acknowledgment')
    if frame[1] == 0x12 and reply[1] == 0x14 and reply[2:4] == b'\0\0':
        return
    if frame[1] == 0x13 and reply[1] == 0x14 and reply[2:4] == frame[2:4] and reply[4] == 1:
        return
    # Commit is a Wi-Fi UART transmit receipt; runtime version below is required.
    if frame[1] == 0x16 and reply[1] == 0x16:
        return
    raise RuntimeError(f'negative/unmatched ACK: {reply.hex()}')


def load_image(image, manifest):
    data = image.read_bytes()
    meta = json.loads(manifest.read_text())
    validate(data)
    if (meta.get('sha256') != hashlib.sha256(data).hexdigest()
            or meta.get('bytes') != len(data) or meta.get('artifact') != image.name
            or meta.get('target') != 'ATSAMD20G18A' or meta.get('link_address') != '0x4000'
            or meta.get('ota_protocol') != 'm1-uart-0x12-0x16'
            or not meta.get('uart_version')):
        raise ValueError('OTA manifest does not match target/image')
    return data, meta


def flash(device, device_id, data, expected_version):
    before = device.identify(device_id)
    if not before.get('uart_ready') or before.get('ota_active') or before.get('wifi_trial_active'):
        raise RuntimeError('device is busy or UART unavailable')
    current = device.command('mcu.version.get')
    if not current.get('version_received') or current.get('updating'):
        raise RuntimeError('MCU version unavailable or upgrade already active')
    same_version = current.get('version') == expected_version
    started_at = time.monotonic()
    print('Target:', device_id, 'Wi-Fi:', before['version'], 'MCU:', current['version'], flush=True)
    packets = list(frames(data))
    started = committed = False
    try:
        for index, packet in enumerate(packets):
            if packet[1] == 0x16:
                # Allow UART owner to consume the separate 0x15 completion event.
                time.sleep(.3)
            last = None
            for attempt in range(3):
                try:
                    if packet[1] == 0x12:
                        started = True  # Cancellation also covers a lost begin ACK.
                    validate_ack(packet, device.exchange(packet))
                    break
                except (OSError, RuntimeError, http.client.HTTPException) as error:
                    last = error
                    if attempt == 2:
                        raise
                    time.sleep(.3)
            if packet[1] == 0x16:
                committed = True
                print('Commit transmitted; waiting for MCU reboot/version proof', flush=True)
            elif packet[1] == 0x13:
                print(f'Block {index}/{len(packets)-2} acknowledged', flush=True)
            time.sleep(.03)
    except BaseException:
        if started and not committed:
            try:
                device.exchange(b'\x23\x16\x01' + bytes(8) + b'\x21')
            except (OSError, RuntimeError, http.client.HTTPException):
                pass
        raise
    until = time.monotonic() + 120
    while time.monotonic() < until:
        try:
            device.identify(device_id)
            current = device.command('mcu.version.get')
            # A same-version upload must not pass on the pre-upgrade cache,
            # including after the Wi-Fi upgrade-state timeout clears.
            age = current.get('version_age_ms')
            fresh = not same_version or (isinstance(age, (int, float)) and
                    not isinstance(age, bool) and 0 <= age < (time.monotonic() - started_at) * 1000)
            if (current.get('version_received') and not current.get('updating') and
                    current.get('version') == expected_version and fresh):
                print('Verified MCU runtime:', json.dumps(current), flush=True)
                return current
        except (OSError, RuntimeError, ValueError, http.client.HTTPException):
            pass
        time.sleep(2)
    raise RuntimeError('commit sent but expected MCU runtime version not confirmed; do not blindly repeat')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--device-id', required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--flash', action='store_true', help='actually transfer and commit; otherwise preflight only')
    args = parser.parse_args()
    data, meta = load_image(args.image, args.manifest)
    device = Device(args.host, os.environ.get('M1_WEB_PASSWORD', ''))
    status = device.identify(args.device_id)
    current = device.command('mcu.version.get')
    print(json.dumps({'device_id': status['device_id'], 'before_mcu': current,
                      'candidate': meta['uart_version'], 'sha256': meta['sha256'], 'bytes': len(data)}), flush=True)
    if args.flash:
        flash(device, args.device_id, data, meta['uart_version'])


if __name__ == '__main__':
    main()
