#!/usr/bin/env python3
"""Check the ARM link fixture without optional Python dependencies."""
import struct
import sys
from pathlib import Path


def check(data):
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
    if vector is None or vector[3] != 0x4000 or vector[5] != 164:
        raise ValueError("wrong application vector location/count")
    words = struct.unpack_from("<41I", data, vector[4])
    if words[0] != 0x20008000 or not (words[1] & 1) or words[1] != entry:
        raise ValueError("wrong stack, reset Thumb bit or ELF entry")
    for i in range(phnum):
        kind, offset, va, pa, filesz, memsz, flags, align = struct.unpack_from("<8I", data, phoff+i*phsize)
        if kind == 1 and filesz and not 0x4000 <= pa <= pa + filesz <= 0x13fff:
            raise ValueError("load segment overlaps reserved Flash")
        if kind == 1 and 0x20000000 <= va and not va + memsz <= 0x20008000:
            raise ValueError("RAM segment exceeds device SRAM")
    return {"vector_base": "0x4000", "vector_words": len(words),
            "initial_sp": hex(words[0]), "reset_vector": hex(words[1]),
            "status": "PASS (link only; hardware not exercised)"}


if __name__ == "__main__":
    import json
    print(json.dumps(check(Path(sys.argv[1]).read_bytes()), indent=2))
