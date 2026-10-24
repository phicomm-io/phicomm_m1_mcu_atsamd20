#!/usr/bin/env python3
"""Package the raw 0x4000 application for MCU UART OTA (never a Wi-Fi OTA image)."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def validate(data):
    n = len(data)
    if not 164 <= n <= 65535:
        raise ValueError('MCU image must be 164..65535 bytes')
    sp, pc = struct.unpack_from('<II', data)
    if not (0x20000000 < sp <= 0x20008000 and sp % 8 == 0 and pc & 1
            and 0x4000 <= pc & ~1 and (pc & ~1) + 2 <= 0x4000 + n):
        raise ValueError('invalid SAMD20 application vector; expected link address 0x4000')
    return n


def crc_block(data):
    crc = 0
    for byte in data:
        for bit in range(7, -1, -1):
            high = crc & 0x8000
            crc = ((crc << 1) | ((byte >> bit) & 1)) & 0xffff
            if high:
                crc ^= 0x1021
    return crc


def frames(data):
    """Begin, data (including mandatory empty tail), commit. No acknowledgments.

    This is a packet generator for a paced sender, NOT a stream to blindly send.
    """
    n = validate(data)
    yield bytes((0x23, 0x12, n // 256, n % 256)) + bytes(7) + b'\x21'
    for index in range(n // 256 + 1):
        chunk = data[index * 256:(index + 1) * 256]
        yield b'\x23\x13' + struct.pack('>H', index + 1) + chunk + struct.pack('>H', crc_block(chunk))
    yield b'\x23\x16' + bytes(9) + b'\x21'


def package(image, source, output):
    data = image.read_bytes()
    n = validate(data)
    record = json.loads(source.read_text())
    digest = hashlib.sha256(data).hexdigest()
    if record['sha256'] != digest or record['bytes'] != n or record['artifact'] != image.name:
        raise ValueError('build manifest does not match application image')
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'm1-mcu-ota.bin'
    binary.write_bytes(data)  # No header, signature, padding or Boot is prepended.
    record.update(artifact=binary.name, format='raw-samd20-application', target='ATSAMD20G18A',
                  link_address='0x4000', staging_address='0x22000',
                  ota_protocol='m1-uart-0x12-0x16', full_blocks=n // 256, tail_bytes=n % 256,
                  data_frames=n // 256 + 1, block_crc='stock-bit-input-0x1021-init0',
                  image_signature=False, hardware_validation='NOT_RUN', release=False,
                  boot_compatibility='stock boot basic copy; open boot requires M1UP journal committed by receiver')
    manifest = output / 'm1-mcu-ota.json'
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    lines = [f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in (binary, manifest)]
    (output / 'SHA256SUMS').write_text(''.join(lines))
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('build/ota'))
    args = parser.parse_args()
    print(json.dumps(package(args.image, args.manifest, args.output), indent=2))
