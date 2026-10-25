#!/usr/bin/env python3
"""Selected private-stock PM paths, SHT status command and PA24 trace.

Executes original instructions only in Unicorn. RTOS, GPIO configuration, NVM
and I2C helpers are stubs; no physical hardware is accessed.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R7, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR
from analyze_stock import STOCK_SHA256

ROOT = Path(__file__).resolve().parents[1]
STOP = 0x10000000


def emulator(blob, mmio=False):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for address, size in [(0, 0x40000), (0x20000000, 0x8000), (STOP, 0x1000)]:
        uc.mem_map(address, size)
    if mmio:
        uc.mem_map(0x41004000, 0x1000)
        uc.mem_map(0xe000e000, 0x1000)
    uc.mem_write(0, blob)
    return uc


def run(uc, address):
    uc.reg_write(UC_ARM_REG_SP, 0x20007fe0)
    uc.reg_write(UC_ARM_REG_LR, STOP | 1)
    uc.emu_start(address | 1, STOP, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == STOP


def pm_frame(value, error=0):
    data = bytearray(32)
    data[:4] = b'\x42\x4d\x00\x1c'
    data[12:14] = struct.pack('>H', value)
    data[29] = error
    data[30:32] = struct.pack('>H', sum(data[:30]))
    return bytes(data)


def pm_compare(blob):
    wrapper = r'''
#include "m1/runtime.h"
static m1_runtime r;
static bool tx(void *p,m1_channel c,const uint8_t *v,size_t n) {(void)p;(void)c;(void)v;(void)n;return true;}
static bool rtc(void *p,m1_clock *c) {(void)p;(void)c;return false;}
void begin(unsigned mode) { m1_io io={.uart=tx,.rtc_read=rtc};m1_runtime_init(&r,&io,0);m1_runtime_pm_mode(&r,(uint8_t)mode,0); }
unsigned feed(const uint8_t *p) {
    r.pm_armed=true;
    for(unsigned i=0;i<32;++i) m1_runtime_rx(&r,M1_PM,p[i],1);
    while(r.pm_armed) m1_runtime_rx(&r,M1_PM,0,1);
    return r.measured.pm25;
}
'''
    cases = 0
    with tempfile.TemporaryDirectory() as d:
        path = Path(d)
        (path / 'wrap.c').write_text(wrapper)
        subprocess.run(['make', '-s', '-C', str(ROOT), 'version'], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC', '-I', str(ROOT / 'include'),
                        *map(str, (ROOT / 'src').glob('*.c')), str(path / 'wrap.c'), '-o', str(path / 'pm.so')], check=True)
        lib = C.CDLL(str(path / 'pm.so'))
        lib.feed.argtypes = [C.c_void_p]
        for mode in (2, 3):
            uc = emulator(blob)
            uc.mem_write(0x200041c4, bytes((mode,)))
            lib.begin(mode)
            # One aligned candidate in each mode-3 64-byte receive window.
            for value, error in [(30, 0)] * 5 + [(1200, 0), (1201, 0), (20, 0), (1000, 0), (1001, 0), (9999, 0), (23, 1), (45, 0)]:
                frame = pm_frame(value, error)
                uc.mem_write(0x20000308 if mode == 2 else 0x200002c8, frame + (bytes(32) if mode == 3 else b''))
                run(uc, 0x78cc)
                want = struct.unpack('<I', uc.mem_read(0x2000404c, 4))[0]
                got = lib.feed(frame)
                assert got == want, (mode, cases, value, error, got, want)
                cases += 1
    return cases


def pa24(blob, start, cancel=0, complete=0):
    uc = emulator(blob, True)
    uc.mem_write(0x200003a2, bytes((cancel,)))
    uc.mem_write(0x200003a0, bytes((complete,)))
    uc.mem_write(0x20004214, struct.pack('<I', 2))
    uc.mem_write(0x20004210, struct.pack('<I', 17))
    uc.reg_write(UC_ARM_REG_R7, 0x16)
    events = []

    def hook(u, address, size, user):
        if address in (0x8b72, 0x8b24):
            u.reg_write(UC_ARM_REG_PC, STOP)
            return
        if address == 0x9ee4:
            events.append(['delay_ticks', u.reg_read(UC_ARM_REG_R0)])
        elif address == 0x84a0:
            events.append(['metadata_commit_stub'])
        elif address not in (0xa448, 0x8dac):
            return
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))

    def write(u, access, address, size, value, user):
        if address in (0x41004414, 0x41004418, 0xe000ed0c):
            events.append([hex(address), hex(value)])
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    run(uc, start)
    return events


def status_command(blob):
    uc = emulator(blob)
    events = []

    def hook(u, address, size, user):
        if address == 0x5764:
            assert u.reg_read(UC_ARM_REG_R0) == 0x44
            command = bytes(u.mem_read(u.reg_read(UC_ARM_REG_R1), 2))
            assert command == b'\xf3\x2d'
            events.append('status-f32d')
            u.reg_write(UC_ARM_REG_R0, 0)
        elif address == 0x57a8:
            assert u.reg_read(UC_ARM_REG_R2) == 3
            u.mem_write(u.reg_read(UC_ARM_REG_R1), b'\x80\x10\x00')
            u.reg_write(UC_ARM_REG_R0, 0)
            events.append('read-3')
        elif address == 0x5660:
            events.append('crc-check')
            u.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, hook)
    run(uc, 0x5868)
    assert events == ['status-f32d', 'read-3', 'crc-check'], events
    return events


def verify(blob):
    assert hashlib.sha256(blob).hexdigest() == STOCK_SHA256
    high = pa24(blob, 0xb578)
    assert ['0x41004418', '0x1000000'] in high
    cancel = pa24(blob, 0x8aae, 1, 1)
    assert not any(e[0] in ('delay_ticks', '0x41004414', '0xe000ed0c') for e in cancel)
    traces = [high, cancel]
    for complete in (0, 1):
        trace = pa24(blob, 0x8aae, 0, complete)
        timing = [e for e in trace if e[0] != 'metadata_commit_stub']
        assert timing == [['delay_ticks', 4000], ['0x41004414', '0x1000000'], ['delay_ticks', 1000], ['0xe000ed0c', '0x5fa0004']], trace
        assert (['metadata_commit_stub'] in trace) == bool(complete)
        traces.append(trace)
    assert struct.unpack_from('<I', blob, 0x9274)[0] == 7999
    return dict(status='PASS', stock_sha256=STOCK_SHA256, pm_comparison_cases=pm_compare(blob),
                pa24_trace_cases=4, pa24_traces=traces, systick_reload=7999,
                sht_status_path=status_command(blob),
                limits='selected instructions; RTOS/GPIO configuration/NVM/I2C are stubs; no hardware proof; mode-3 comparison uses one candidate per 64-byte window')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.image.read_bytes()), indent=2))
