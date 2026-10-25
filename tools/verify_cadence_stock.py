#!/usr/bin/env python3
"""Trace scheduling and sample publication in a private stock dump, without I/O."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP
from analyze_stock import STOCK_SHA256
from verify_peripherals_stock import emulator, run, STOP
ROOT = Path(__file__).resolve().parents[1]


def scheduler(blob):
    uc = emulator(blob)
    handles = {0x200043e4: 'hcho', 0x20004400: 'climate', 0x200043f4: 'pm',
               0x200043fc: 'render', 0x200043e0: 'rtc'}
    for address in handles:
        uc.mem_write(address, struct.pack('<I', address))
    time = 0
    events = {name: [] for name in handles.values()}

    def hook(u, address, size, user):
        nonlocal time
        if address == 0x9648:
            events[handles[u.reg_read(UC_ARM_REG_R0)]].append(time)
        elif address == 0x4f18:
            pass  # watchdog service, no MMIO
        elif address == 0x9ee4:
            assert u.reg_read(UC_ARM_REG_R0) == 1000
            time += 1000
            if time == 15000:
                u.reg_write(UC_ARM_REG_PC, STOP)
                return
        else:
            return
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    run(uc, 0x7594)
    for name in ('hcho', 'climate', 'rtc'):
        assert events[name] == list(range(0, 15000, 1000))
    for name in ('pm', 'render'):
        assert events[name] == [0, 3000, 6000, 9000, 12000]
    return events


def screen_timer(blob):
    uc = emulator(blob)
    config = {}
    def creation(u, address, size, user):
        if address == 0x71b4:
            pass  # TM1680 hardware initialization
        elif address == 0xa3fc:
            config.update(period=u.reg_read(UC_ARM_REG_R1), repeat=u.reg_read(UC_ARM_REG_R2),
                          callback=struct.unpack('<I', u.mem_read(u.reg_read(UC_ARM_REG_SP), 4))[0] & ~1)
            u.reg_write(UC_ARM_REG_R0, 1)
        elif address in (0x9ca0, 0xa448):
            u.reg_write(UC_ARM_REG_R0, 0)
        elif address == 0x9824:
            u.reg_write(UC_ARM_REG_PC, STOP)
            return
        else:
            return
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
    handle = uc.hook_add(UC_HOOK_CODE, creation)
    run(uc, 0x7320)
    uc.hook_del(handle)
    assert config == dict(period=100, repeat=1, callback=0x7284)
    at = 0
    writes = []
    def timer(u, address, size, user):
        if address == 0x7148:
            writes.append(at)
        elif address not in (0x6594, 0x6638):
            return
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, timer)
    for at in range(100, 13001, 100):
        run(uc, config['callback'])
    assert writes == [2200, 4300, 6400, 8500, 10600, 12700], writes
    return dict(period_ticks=100, send_ticks=writes, steady_period_ticks=2100)


def renderer(blob):
    uc = emulator(blob)
    uc.mem_write(0x20000000, blob[0xbed4:0xc07c])
    uc.mem_write(0x2000027c, struct.pack('<I', 0x20006000))
    def helpers(u, address, size, user):
        if address in (0xb858, 0xb964):
            a, b = u.reg_read(UC_ARM_REG_R0), u.reg_read(UC_ARM_REG_R1)
            u.reg_write(UC_ARM_REG_R0, a//b)
            u.reg_write(UC_ARM_REG_R1, a % b)
        elif address == 0x9790:
            u.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, helpers)
    return uc


def snapshot(blob):
    uc = renderer(blob)
    rows = []
    for hcho, pm, temperature, humidity in [(41, 12, (0, 23, 45), (56, 78)),
                                           (176, 81, (1, 5, 50), (63, 21)),
                                           (5, 1600, (0, 27, 19), (99, 99))]:
        uc.mem_write(0x2000408c, struct.pack('<I', hcho))
        uc.mem_write(0x2000404c, struct.pack('<I', pm))
        uc.mem_write(0x20004054, bytes(temperature))
        uc.mem_write(0x20004090, bytes(humidity))
        before = bytes(uc.mem_read(0x20004080, 24))
        run(uc, 0x72d4)
        after = bytes(uc.mem_read(0x20004080, 24))
        want_hcho = 10 if hcho < 11 else hcho
        assert struct.unpack_from('<II', after) == (pm, want_hcho)
        assert after[8:10] == bytes(humidity) and after[20:23] == bytes(temperature)
        rows.append(dict(hcho=hcho, pm=pm, published_hcho=want_hcho,
                         temperature=list(temperature), humidity=list(humidity)))
        # Sensor RAM changes do not modify the reply cache until rendering.
        uc.mem_write(0x2000404c, struct.pack('<I', 999))
        assert bytes(uc.mem_read(0x20004080, 24)) == after
        assert before != after
    return rows


def runtime_compare(blob, schedule, timer):
    wrapper = r'''
#include "m1/runtime.h"
#include <string.h>
static m1_runtime r;
static unsigned writes;
static uint8_t ram[29];
static bool tx(void *p,m1_channel c,const uint8_t *d,size_t n) {(void)p;(void)c;(void)d;(void)n;return true;}
static bool rtc(void *p,m1_clock *c) {(void)p;(void)c;return false;}
static bool i2c(void *p,unsigned bus,uint8_t addr,const uint8_t *d,size_t n,uint8_t *out,size_t nr) {
    (void)p;(void)addr;(void)out;(void)nr;
    if(bus==0 && n==30) {memcpy(ram,d+1,29);writes++;} return bus==0;
}
static void nop(void *p) {(void)p;}
static void buzzer(void *p,bool on) {(void)p;(void)on;}
void begin(void) {
    m1_io io={.uart=tx,.i2c=i2c,.rtc_read=rtc,.reset=nop,.buzzer=buzzer};
    m1_runtime_init(&r,&io,0);writes=0;
}
void step(unsigned now,unsigned hcho,unsigned pm,int t,unsigned rh) {
    r.state.hcho_raw=hcho;r.measured.pm25=pm;
    r.measured.temperature_centi_c=t;r.measured.humidity_centi_pct=rh;
    r.climate_at=now+60000;m1_runtime_poll(&r,now,false);
}
int value(unsigned field) {
    switch(field) {
    case 0:return r.state.sample.hcho;case 1:return r.state.sample.pm25;
    case 2:return r.state.sample.temperature_centi_c;case 3:return r.state.sample.humidity_centi_pct;
    default:return writes;
    }
}
void screen(uint8_t *out) {memcpy(out,ram,29);}
'''
    uc = renderer(blob)
    for address, value in ((0x200001dc, 0), (0x200001d4, 1), (0x200001d8, 1),
                           (0x200001db, 0), (0x200001df, 0)):
        uc.mem_write(address, bytes((value,)))
    with tempfile.TemporaryDirectory(prefix='m1-cadence-') as d:
        path = Path(d)
        (path/'wrapper.c').write_text(wrapper)
        subprocess.run(['make', '-s', '-C', str(ROOT), 'version'], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-I', str(ROOT/'include'),
                        *map(str, sorted((ROOT/'src').glob('*.c'))), str(path/'wrapper.c'),
                        '-o', str(path/'runtime.so')], check=True)
        lib = C.CDLL(str(path/'runtime.so'))
        lib.step.argtypes = [C.c_uint, C.c_uint, C.c_uint, C.c_int, C.c_uint]
        lib.screen.argtypes = [C.c_void_p]
        lib.begin()
        expected = (0, 0, 0, 0)
        screen_times = [0]+timer['send_ticks']
        display_ram = bytes(29)
        actual_times = []
        for ms in range(13001):
            hcho, pm, temperature, humidity = (150+ms % 997, ms % 211, -550+ms % 100, 5500+ms % 79) if ms else (0, 0, 0, 0)
            if ms in schedule['render']:
                uc.mem_write(0x2000408c, struct.pack('<I', hcho))
                uc.mem_write(0x2000404c, struct.pack('<I', pm))
                t = abs(temperature)
                uc.mem_write(0x20004054, bytes((temperature < 0, t//100, t % 100)))
                uc.mem_write(0x20004090, bytes((humidity//100, humidity % 100)))
                run(uc, 0x72d4)
                cache = bytes(uc.mem_read(0x20004080, 24))
                expected = (struct.unpack_from('<I', cache, 4)[0], struct.unpack_from('<I', cache)[0],
                            (-1 if cache[20] else 1)*(cache[21]*100+cache[22]), cache[8]*100+cache[9])
                display_ram = bytes(uc.mem_read(0x20006000, 29))
            lib.step(ms, hcho, pm, temperature, humidity)
            got = tuple(lib.value(i) for i in range(4))
            assert got == expected, (ms, got, expected)
            if lib.value(4) > len(actual_times):
                actual_times.append(ms)
                actual = (C.c_uint8*29)()
                lib.screen(actual)
                assert bytes(actual) == display_ram, ('screen', ms, bytes(actual).hex(), display_ram.hex())
            assert len(actual_times) == len([t for t in screen_times if t <= ms])
        assert actual_times == screen_times
    return dict(snapshot_comparisons=13001, screen_transfers=actual_times,
                screen_bytes_compared=len(actual_times)*29, status='PASS')


def verify(blob):
    if hashlib.sha256(blob).hexdigest() != STOCK_SHA256:
        raise ValueError('addresses only apply to the documented stock SHA-256')
    schedule, timer = scheduler(blob), screen_timer(blob)
    return dict(status='PASS', stock_sha256=STOCK_SHA256, schedule=schedule,
                screen_timer=timer, sample_cache=snapshot(blob),
                runtime_comparison=runtime_compare(blob, schedule, timer),
                limits='original ARM instructions with RTOS/I2C helpers stubbed; tick-to-ms uses static clock configuration; no hardware proof')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.image.read_bytes()), indent=2))
