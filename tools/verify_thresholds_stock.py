#!/usr/bin/env python3
"""Verify stock 0x05/0x06 RAM writes using a private MCU backup; no hardware I/O."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import UC_ARM_REG_PC
from analyze_stock import STOCK_SHA256
from verify_peripherals_stock import emulator, run, STOP
from verify_cadence_stock import renderer
from verify_runtime_stock import State

ROOT = Path(__file__).resolve().parents[1]


def compare_native(rows):
    with tempfile.TemporaryDirectory(prefix='m1-threshold-') as directory:
        library = Path(directory)/'core.so'
        subprocess.run(['make', '-s', '-C', str(ROOT), 'version'], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC', '-I', str(ROOT/'include'),
                        str(ROOT/'src/core.c'), '-o', str(library)], check=True)
        lib = C.CDLL(str(library))
        lib.m1_init.argtypes = [C.POINTER(State)]
        lib.m1_command.argtypes = [C.POINTER(State), C.c_void_p, C.c_void_p]
        comparisons = rejected = 0
        for command in (5, 6):
            state = State()
            lib.m1_init(C.byref(state))
            for low, high in rows:
                frame = bytes((0x23, command)) + struct.pack('>HH', low, high) + bytes(5) + b'\x21'
                before = bytes(state)
                reply = (C.c_uint8*20)()
                result = lib.m1_command(C.byref(state), frame, reply)
                if low > high:
                    assert result == 0 and bytes(state) == before  # M1_BAD_FRAME
                    rejected += 1
                else:
                    assert result == 2  # M1_APPLIED
                    assert tuple(state.ht if command == 5 else state.pt) == (low, high)
                    comparisons += 1
        return comparisons, rejected


def verify(blob):
    if hashlib.sha256(blob).hexdigest() != STOCK_SHA256:
        raise ValueError('addresses only apply to the documented stock SHA-256')
    rng = random.Random(20261025)
    rows = [(0, 0), (0, 65535), (65535, 65535), (65535, 0), (100, 300), (75, 150)]
    rows += [(rng.randrange(65536), rng.randrange(65536)) for _ in range(128)]
    cases = 0
    locations = {5: (0x200003a4, 0x2000039c, 0x200003b0),
                 6: (0x200003a8, 0x200003ac, 0x200003a1)}
    for command in (5, 6):
        uc = emulator(blob)
        def stop(u, address, size, user):
            if address == 0x7fa4:
                u.reg_write(UC_ARM_REG_PC, STOP)
        uc.hook_add(UC_HOOK_CODE, stop)
        for low, high in rows:
            # Dispatch saves the command at SP+15 before the selected block.
            uc.mem_write(0x20007fef, bytes((command,)))
            uc.mem_write(0x2000008c, bytes((0x23, command)) + struct.pack('>HH', low, high) + bytes(5) + b'\x21')
            for addresses in locations.values():
                for address, width in zip(addresses, (4, 4, 1)):
                    uc.mem_write(address, bytes(width))
            run(uc, 0x7f50)
            a, b, flag = locations[command]
            assert struct.unpack('<I', uc.mem_read(a, 4))[0] == low
            assert struct.unpack('<I', uc.mem_read(b, 4))[0] == high
            assert bytes(uc.mem_read(flag, 1)) == b'\x01'
            for address, width in zip(locations[11-command], (4, 4, 1)):
                assert bytes(uc.mem_read(address, width)) == bytes(width)
            cases += 1

    # Rendering uses immediate constants even after the threshold RAM changes.
    uc = renderer(blob)
    reads = []
    watched = {address for addresses in locations.values() for address in addresses}
    def read(u, access, address, size, value, user):
        if any(address <= target < address+size for target in watched):
            reads.append(address)
    uc.hook_add(UC_HOOK_MEM_READ, read)
    render_cases = 0
    for hcho in (0, 80, 100, 101, 300, 301):
        for pm in (0, 35, 75, 76, 150, 151):
            masks = []
            for low, high in ((10, 20), (1000, 2000)):
                for a, b, flag in locations.values():
                    uc.mem_write(a, struct.pack('<I', low))
                    uc.mem_write(b, struct.pack('<I', high))
                    uc.mem_write(flag, b'\x01')
                uc.mem_write(0x2000408c, struct.pack('<I', hcho))
                uc.mem_write(0x2000404c, struct.pack('<I', pm))
                run(uc, 0x72d4)
                ram = bytes(uc.mem_read(0x20006000, 29))
                masks.append(tuple(ram[i] & 0xf5 for i in (0x0c, 0x13, 0x12)))
                render_cases += 1
            assert masks[0] == masks[1], (hcho, pm, masks)
    assert not reads, reads
    comparisons, rejected = compare_native(rows)
    return dict(status='PASS', stock_sha256=STOCK_SHA256, write_cases=cases,
                stock_accepts_equal_reversed_and_zero=True,
                native_valid_write_comparisons=comparisons, native_reversed_pair_rejections=rejected,
                render_cases=render_cases, threshold_reads_during_render=len(reads),
                threshold_ram_changes_do_not_change_stock_lamp_groups=True,
                limits='selected ARM dispatch/render blocks with helpers stubbed; no full-program consumer proof or hardware validation')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.image.read_bytes()), indent=2))
