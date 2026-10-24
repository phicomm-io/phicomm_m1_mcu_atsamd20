#!/usr/bin/env python3
"""Inspect a user-supplied dump; never fetch or publish stock firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

STOCK_SHA256 = "6a8a9f72741e01d5ebc73467819079146553ec73339029eaf4cc49e8cbd1a7f9"


def analyze(blob):
    if len(blob) != 0x40000:
        raise ValueError("expected a 262144-byte SAM D20G18A main-Flash dump")
    digest = hashlib.sha256(blob).hexdigest()
    vectors = []
    for base in (0, 0x4000):
        sp, reset = struct.unpack_from("<II", blob, base)
        vectors.append({"base": hex(base), "initial_sp": hex(sp), "reset_vector": hex(reset),
                        "plausible": 0x20000000 < sp <= 0x20008000 and
                        bool(reset & 1) and base <= (reset & ~1) < len(blob)})
    used = [i for i, v in enumerate(blob) if v != 0xff]
    strings = [{"address": hex(m.start()), "text": m.group().decode("ascii")}
               for m in re.finditer(rb"[\x20-\x7e]{5,}", blob)
               if m.group().endswith((b"_task", b"timer")) or
               m.group() in (b"11.1.2.1", b"i2c_th", b"i2c_rtc", b"up_sub_task", b"Tmr Svc")]
    result = {"size": len(blob), "sha256": digest, "known_snapshot": digest == STOCK_SHA256,
              "vectors": vectors, "last_non_ff": hex(used[-1]) if used else None,
              "non_ff_blocks_4k": [hex(a) for a in range(0, len(blob), 4096)
                                   if blob[a:a+4096] != b"\xff" * 4096],
              "selected_strings": strings}
    if digest == STOCK_SHA256:
        tasks = []
        # Consecutive name / entry literals used by xTaskCreate-style calls in main.
        for address in (0xb800,0xb80c,0xb814,0xb81c,0xb824,0xb82c,0xb834,0xb83c,0xb844,0xb84c):
            name, entry = struct.unpack_from("<II", blob, address)
            tasks.append({"name": blob[name:blob.index(0, name)].decode("ascii"),
                          "entry": hex(entry & ~1), "literal_pair": hex(address)})
        result["tasks"] = tasks
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.image.read_bytes())
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
