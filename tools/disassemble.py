#!/usr/bin/env python3
"""Local Thumb disassembly with PC literal annotations. Requires capstone."""
import argparse
from pathlib import Path
import struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
from capstone.arm import ARM_OP_MEM, ARM_REG_PC

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("image", type=Path)
p.add_argument("start", type=lambda v: int(v, 0))
p.add_argument("end", type=lambda v: int(v, 0))
a = p.parse_args()
data = a.image.read_bytes()
if not 0 <= a.start < a.end <= len(data):
    p.error("address range is outside this zero-based image")
decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
decoder.detail = True
for ins in decoder.disasm(data[a.start:a.end], a.start):
    note = ""
    if ins.mnemonic == "ldr" and len(ins.operands) > 1:
        op = ins.operands[1]
        if op.type == ARM_OP_MEM and op.mem.base == ARM_REG_PC:
            address = ((ins.address + 4) & ~3) + op.mem.disp
            if 0 <= address <= len(data) - 4:
                value = struct.unpack_from("<I", data, address)[0]
                note = f" ; [{address:#x}] = {value:#010x}"
    print(f"{ins.address:08x}: {ins.mnemonic:8} {ins.op_str}{note}")
