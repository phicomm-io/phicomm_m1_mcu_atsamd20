#!/usr/bin/env python3
"""Execute the built ARM clock routine against a small SAM D20 MMIO model.

This checks generator bank widths, division modes and WDT synchronization.
It is not a cycle-accurate SoC emulator or a physical startup test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

STOP = 0x10000000
GCLK = 0x40000c00
WDT = 0x40001000


def load_elf(uc, data):
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected little-endian ELF32')
    h = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if h[2] != 40:
        raise ValueError('expected EM_ARM')
    for i in range(h[10]):
        kind, off, va, _, size, *_ = struct.unpack_from('<8I', data, h[5] + i*h[9])
        if kind == 1 and size:
            uc.mem_write(va, data[off:off+size])
    sections = [struct.unpack_from('<10I', data, h[6]+i*h[11]) for i in range(h[12])]
    for s in sections:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[s[6]]
        names = data[strings[4]:strings[4]+strings[5]]
        for off in range(s[4], s[4]+s[5], s[9]):
            name, value, *_ = struct.unpack_from('<IIIBBH', data, off)
            if names[name:].split(b'\0', 1)[0] == b'm1_platform_clock':
                return value
    raise ValueError('missing m1_platform_clock symbol')


class ClockModel:
    def __init__(self, fault=None, warm=False, always_on=False):
        self.fault = fault
        self.div = {0: 0, 1: 0, 2: 0}
        self.gen = {0: 0x10600, 2: 0x10302}
        self.selected = {GCLK+4: 0, GCLK+8: 0}
        self.channels = {}
        self.gbusy = self.wbusy = 0
        self.ctrl = (0x82 if always_on else 2) if warm or always_on else 0
        self.config = 9 if always_on else 11
        self.initial_config = self.config
        self.clears = 0

    def read(self, uc, access, address, size, value, user):
        result = None
        if address == GCLK+1:
            result = int(self.gbusy > 0) << 7
            self.gbusy = max(0, self.gbusy-1)
        elif address == WDT+7:
            result = int(self.wbusy > 0) << 7
            self.wbusy = max(0, self.wbusy-1)
        elif address == GCLK+8:
            bank = self.selected[address]
            result = bank | (self.div[bank] << 8)
        elif address == GCLK+4:
            result = self.gen[self.selected[address]]
        elif address == WDT:
            result = self.ctrl
        elif address == WDT+1:
            result = self.config
        if result is not None:
            uc.mem_write(address, result.to_bytes(size, 'little'))

    def write(self, uc, access, address, size, value, user):
        if address in (GCLK+2, GCLK+4, GCLK+8):
            assert not self.gbusy, 'GCLK written before synchronization'
            self.gbusy = 3
        if address == GCLK+8:
            bank = value & 15
            self.selected[address] = bank
            # DS60001504, 14.8.5: GCLK0=8, GCLK1=16, GCLK2=5 bits.
            bits = {0: 8, 1: 16, 2: 5}[bank]
            self.div[bank] = (value >> 8) & ((1 << bits)-1)
            if bank == 2 and self.fault == 'divider_write_lost':
                self.div[bank] = 0
        elif address == GCLK+4:
            bank = value & 15
            self.selected[address] = bank
            self.gen[bank] = value
            if bank == 2 and self.fault == 'divsel_write_lost':
                self.gen[bank] &= ~(1 << 20)
        elif address == GCLK+2:
            self.channels[value & 63] = value
        elif address in (WDT, WDT+1, WDT+8):
            assert not self.wbusy, 'WDT written before synchronization'
            if address == WDT:
                assert not self.ctrl & 0x80, 'must preserve ALWAYSON configuration'
                self.ctrl = value
            elif address == WDT+1:
                assert not self.ctrl & 2, 'CONFIG is enable-protected'
                self.config = value
            else:
                assert value == 0xa5, 'invalid watchdog clear key'
                self.clears += 1
            self.wbusy = 3

    def frequency(self, bank):
        gen = self.gen[bank]
        assert gen & (1 << 16), 'generator disabled'
        source = {3: 32768, 6: 8000000}[(gen >> 8) & 31]
        divisor = (1 << (self.div[bank]+1)) if gen & (1 << 20) else max(1, self.div[bank])
        return source / divisor


def execute(data, **options):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for address, size in ((0, 0x40000), (0x20000000, 0x8000), (STOP, 0x1000),
                          (0x40000000, 0x2000), (0x41004000, 0x1000), (0xe000e000, 0x1000)):
        uc.mem_map(address, size)
    entry = load_elf(uc, data)
    uc.mem_write(0x4000080c, struct.pack('<I', 1 << 3))  # OSC8MRDY
    model = ClockModel(**options)
    uc.hook_add(UC_HOOK_MEM_READ, model.read, begin=0x40000000, end=0x40001fff)
    uc.hook_add(UC_HOOK_MEM_WRITE, model.write, begin=0x40000000, end=0x40001fff)
    uc.reg_write(UC_ARM_REG_SP, 0x20007ff0)
    uc.reg_write(UC_ARM_REG_LR, STOP | 1)
    uc.emu_start(entry | 1, STOP, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == STOP, 'clock routine did not return within budget'
    return uc.reg_read(UC_ARM_REG_R0), model, uc


def verify(path):
    data = path.read_bytes()
    for warm, always_on in ((False, False), (True, False), (True, True)):
        result, model, uc = execute(data, warm=warm, always_on=always_on)
        assert result == 1, 'clock initialization failed'
        assert model.frequency(0) == 8000000, 'wrong main clock'
        assert model.frequency(2) == 1024, 'GCLK2 must supply nominal 1024 Hz'
        assert model.channels[1] == 0x4201, 'WDT not enabled on GCLK2'
        assert model.ctrl & 2 and model.clears == 1, 'watchdog not started/cleared'
        if always_on:
            assert model.config == model.initial_config, 'ALWAYSON period was changed'
        else:
            assert model.config == 10, 'watchdog must use 8192 cycles'
            assert (8 << model.config) / model.frequency(2) == 8, 'wrong WDT period'
        assert struct.unpack('<I', uc.mem_read(0xe000e014, 4))[0] == 7999, 'wrong 1 ms SysTick'
    for fault in ('divider_write_lost', 'divsel_write_lost'):
        result, model, _ = execute(data, fault=fault)
        assert result == 0 and not model.clears, 'must reject incorrect GCLK2 readback'
    return {'elf': path.name, 'sha256': hashlib.sha256(data).hexdigest(), 'cases': 5,
            'gclk2_hz_nominal': 1024, 'wdt_seconds_nominal': 8,
            'status': 'PASS (ARM instructions + MMIO model; hardware NOT_RUN)'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path, nargs='+')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = [verify(path) for path in args.elf]
    encoded = json.dumps(report, indent=2) + '\n'
    if args.report:
        args.report.write_text(encoded)
    print(encoded, end='')
