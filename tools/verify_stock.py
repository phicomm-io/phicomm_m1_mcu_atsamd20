#!/usr/bin/env python3
"""Differential checks against a private stock dump. No hardware access.

Requires unicorn==2.1.4 and a host C compiler. Stock code is executed only in
emulated Flash/SRAM. No MMIO is mapped; scheduler/format helpers are stubbed.
"""
import argparse
import ctypes as ct
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
from analyze_stock import STOCK_SHA256

ROOT = Path(__file__).resolve().parents[1]
STOP = 0x10000000
DISPLAY = 0x20006000


class Sample(ct.Structure):
    _fields_ = [("hcho", ct.c_uint16), ("pm25", ct.c_uint16),
                ("temperature", ct.c_int16), ("humidity", ct.c_uint16),
                ("pm_health", ct.c_uint8), ("jq_health", ct.c_uint8), ("th_health", ct.c_uint8)]


def verify(blob):
    if hashlib.sha256(blob).hexdigest() != STOCK_SHA256:
        raise ValueError("function addresses apply only to the documented stock SHA-256")
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0, 0x40000); uc.mem_write(0, blob)
    uc.mem_map(0x20000000, 0x8000)
    uc.mem_map(STOP, 0x1000)
    uc.mem_write(0x20000000, blob[0xbed4:0xc07c])
    uc.mem_write(0x2000027c, struct.pack("<I", DISPLAY))

    def helpers(emulator, address, size, user):
        if address in (0xb858, 0xb964):
            a = emulator.reg_read(UC_ARM_REG_R0); b = emulator.reg_read(UC_ARM_REG_R1)
            assert b
            emulator.reg_write(UC_ARM_REG_R0, a // b)
            emulator.reg_write(UC_ARM_REG_R1, a % b)
        elif address in (0x66a0, 0x9790):
            emulator.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        emulator.reg_write(UC_ARM_REG_PC, emulator.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, helpers)

    def run(address, a=0, b=0, end=STOP):
        uc.reg_write(UC_ARM_REG_SP, 0x20007ff0)
        uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        uc.reg_write(UC_ARM_REG_R0, a); uc.reg_write(UC_ARM_REG_R1, b)
        uc.emu_start(address | 1, end, count=10000)
        assert uc.reg_read(UC_ARM_REG_PC) == end, "instruction budget exceeded"
        return uc.reg_read(UC_ARM_REG_R0)

    with tempfile.TemporaryDirectory(prefix="m1-core-verify-") as tmp:
        libpath = Path(tmp) / "core.so"
        subprocess.run(["cc", "-std=c11", "-O2", "-shared", "-fPIC", "-I", str(ROOT/"include"),
                        str(ROOT/"src/core.c"), "-o", str(libpath)], check=True)
        lib = ct.CDLL(str(libpath))
        lib.m1_stock_crc_byte.argtypes = [ct.c_uint16, ct.c_uint8]
        lib.m1_stock_crc_byte.restype = ct.c_uint16
        lib.m1_sht_crc.argtypes = [ct.c_void_p, ct.c_size_t]; lib.m1_sht_crc.restype = ct.c_uint8
        lib.m1_display_groups.argtypes = [ct.c_void_p, ct.c_uint, ct.c_uint]
        lib.m1_sample_reply.argtypes = [ct.POINTER(Sample), ct.c_void_p]
        rng = random.Random(20261024)
        crc_cases = [(0, x) for x in range(256)] + [(rng.randrange(65536), rng.randrange(256)) for _ in range(256)]
        for crc, byte in crc_cases:
            assert run(0x84e0, crc, byte) == lib.m1_stock_crc_byte(crc, byte)
        for _ in range(64):
            data = rng.randbytes(2); buf = ct.create_string_buffer(data)
            uc.mem_write(0x20007000, data)
            assert run(0x5660, 0x20007000, 2) == lib.m1_sht_crc(buf, 2)
        group_cases = 0
        for h in (1,2,3,9):
            for p in (1,2,3,9):
                initial = rng.randbytes(29)
                uc.mem_write(DISPLAY, initial)
                run(0x69c4, h); run(0x6c90, p)
                out = (ct.c_uint8 * 29).from_buffer_copy(initial)
                lib.m1_display_groups(out, h, p)
                assert bytes(out) == bytes(uc.mem_read(DISPLAY, 29))
                group_cases += 1
        render_cases = 0
        for address, global_addr, func, mask in (
                (0x6ad8,0x2000408c,lib.m1_hcho_group,0x70),
                (0x6dac,0x2000404c,lib.m1_pm25_group,0x85)):
            for value in (0,10,11,74,75,76,100,101,149,150,151,299,300,301,998,999,1000,9999,10000):
                uc.mem_write(DISPLAY, bytes(29)); uc.mem_write(global_addr, struct.pack("<I", value))
                run(address)
                ram = uc.mem_read(DISPLAY, 29)
                # Date/numeric bits can coexist: compare only group masks.
                actual = [ram[a] & mask for a in (0x0c,0x13,0x12)]
                expected = [mask if i+1 == func(value) else 0 for i in range(3)]
                assert actual == expected, (hex(address), value, actual, expected)
                render_cases += 1
        s = Sample(1234,432,-550,4567,1,2,1)
        uc.mem_write(0x20004084, struct.pack("<I", s.hcho))
        uc.mem_write(0x20004080, struct.pack("<I", s.pm25))
        uc.mem_write(0x20004094, bytes((1,5,50)))
        uc.mem_write(0x20004088, bytes((45,67)))
        for a,v in ((0x20000328,1),(0x200002ad,2),(0x200001f0,1)):
            uc.mem_write(a, bytes((v,)))
        uc.mem_write(0x20000388, bytes(20))
        run(0x85a0, end=0x85ec)
        out = (ct.c_uint8 * 20)(); lib.m1_sample_reply(ct.byref(s), out)
        assert bytes(out) == bytes(uc.mem_read(0x20000388,20))
    return {"stock_sha256": STOCK_SHA256, "status": "PASS",
            "crc16_cases": len(crc_cases), "sht_crc_cases": 64,
            "display_mask_cases": group_cases, "display_boundary_cases": render_cases,
            "sample_frame_cases": 1,
            "limits": "pure routines/selected blocks; scheduler helpers stubbed; no hardware or OTA tested"}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__); p.add_argument("image", type=Path)
    a = p.parse_args()
    print(json.dumps(verify(a.image.read_bytes()), indent=2))
