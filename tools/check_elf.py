#!/usr/bin/env python3
"""Check the ARM link fixture without optional Python dependencies."""
import struct
import sys
from pathlib import Path


def check(data, boot=False):
    base, limit = (0, 0x4000) if boot else (0x4000, 0x13fff)
    if data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected little-endian ELF32")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if header[2] != 40:
        raise ValueError("expected EM_ARM")
    entry, phoff, shoff = header[4:7]
    phsize, phnum, shsize, shnum, names_idx = header[9:14]
    sections = [struct.unpack_from("<10I", data, shoff + i * shsize) for i in range(shnum)]
    ns = sections[names_idx]
    names = data[ns[4]:ns[4]+ns[5]]
    vector = None
    for section in sections:
        n = names[section[0]:].split(b"\0", 1)[0]
        if n == b".vectors":
            vector = section
    if vector is None or vector[3] != base or vector[5] != 164:
        raise ValueError("wrong application vector location/count")
    words = struct.unpack_from("<41I", data, vector[4])
    if words[0] != 0x20008000 or not (words[1] & 1) or words[1] != entry:
        raise ValueError("wrong stack, reset Thumb bit or ELF entry")
    reserved = {4,5,6,7,8,9,10,12,13,35,36}
    for i, address in enumerate(words[1:],1):
        if i in reserved:
            if address: raise ValueError("reserved interrupt vector is not zero")
        elif not (address & 1 and base <= (address & ~1) < limit):
            raise ValueError("handler is not a Thumb address in image Flash")
    for i in ((15,) if boot else (15,20,24,26,27)):
        if words[i] == words[2]:
            raise ValueError("runtime interrupt still uses Default_Handler")
    for i in range(phnum):
        kind, offset, va, pa, filesz, memsz, flags, align = struct.unpack_from("<8I", data, phoff+i*phsize)
        if kind == 1 and filesz and not base <= pa <= pa + filesz <= limit:
            raise ValueError("load segment overlaps reserved Flash")
        if kind == 1 and 0x20000000 <= va and not va + memsz <= 0x20008000:
            raise ValueError("RAM segment exceeds device SRAM")
    return {"vector_base": hex(base), "vector_words": len(words),
            "initial_sp": hex(words[0]), "reset_vector": hex(words[1]),
            "status": "PASS (link only; hardware not exercised)"}


if __name__ == "__main__":
    import json
    print(json.dumps(check(Path(sys.argv[1]).read_bytes(), "--boot" in sys.argv), indent=2))
