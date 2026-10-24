#!/usr/bin/env python3
"""Private-dump differential test for compensation and all 29 display bytes.

The original arithmetic/font instructions execute in Unicorn. Only sensor I/O,
unsigned division and semaphore helpers are replaced. No hardware is accessed.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR
from analyze_stock import STOCK_SHA256
from verify_stock import Sample, ROOT, STOP, DISPLAY

class Clock(C.Structure):
    _fields_=[('year',C.c_uint16)]+[(n,C.c_uint8) for n in ('month','day','hour','minute','second')]
class State(C.Structure):
    _fields_=[('sample',Sample),('clock',Clock),('ht',C.c_uint16*2),('pt',C.c_uint16*2),('raw',C.c_uint16),('brightness',C.c_uint8),('on',C.c_bool),('buzzer',C.c_bool),('test',C.c_bool)]
class Climate(C.Structure):
    _fields_=[('x',C.c_int64*4),('heat',C.c_int64),('aux',C.c_int64),('initialized',C.c_bool)]

def verify(blob):
    assert hashlib.sha256(blob).hexdigest()==STOCK_SHA256
    u=Uc(UC_ARCH_ARM,UC_MODE_THUMB); u.mem_map(0,0x40000);u.mem_write(0,blob)
    u.mem_map(0x20000000,0x8000);u.mem_map(STOP,0x1000)
    u.mem_write(0x20000000,blob[0xbed4:0xc07c]); u.mem_write(0x2000027c,struct.pack('<I',DISPLAY))
    inputs=[26000,32000,0x12345678]
    def hook(uc,at,size,user):
        if at in (0xb858,0xb964):
            a=uc.reg_read(UC_ARM_REG_R0);b=uc.reg_read(UC_ARM_REG_R1);assert b
            uc.reg_write(UC_ARM_REG_R0,a//b);uc.reg_write(UC_ARM_REG_R1,a%b)
        elif at==0x57ec:
            uc.mem_write(uc.reg_read(UC_ARM_REG_R1),struct.pack('<I',inputs[0]));uc.mem_write(uc.reg_read(UC_ARM_REG_R2),struct.pack('<I',inputs[1]));uc.reg_write(UC_ARM_REG_R0,0)
        elif at==0x57a8:
            a=inputs[2];uc.mem_write(uc.reg_read(UC_ARM_REG_R1),bytes((a>>24,(a>>16)&255,0,(a>>8)&255,a&255,0)));uc.reg_write(UC_ARM_REG_R0,0)
        elif at in (0x5764,0x642c,0x9790): uc.reg_write(UC_ARM_REG_R0,0)
        else:return
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    u.hook_add(UC_HOOK_CODE,hook)
    def run(at,a=0,b=0):
        u.reg_write(UC_ARM_REG_SP,0x20007ff0);u.reg_write(UC_ARM_REG_LR,STOP|1)
        u.reg_write(UC_ARM_REG_R0,a);u.reg_write(UC_ARM_REG_R1,b)
        u.emu_start(at|1,STOP,count=200000)
        assert u.reg_read(UC_ARM_REG_PC)==STOP,hex(at)
    with tempfile.TemporaryDirectory() as tmp:
        libpath=Path(tmp)/'runtime.so'
        subprocess.run(['make','-s','-C',str(ROOT),'version'],check=True,stdout=subprocess.DEVNULL)
        subprocess.run(['cc','-std=c11','-O2','-shared','-fPIC','-I',str(ROOT/'include'),*[str(ROOT/'src'/f'{n}.c') for n in ('core','display','climate')],'-o',str(libpath)],check=True)
        lib=C.CDLL(str(libpath));lib.m1_display_render.argtypes=[C.POINTER(State),C.c_void_p]
        lib.m1_init.argtypes=[C.POINTER(State)]
        lib.m1_climate_convert.argtypes=[C.POINTER(Climate),C.c_uint16,C.c_uint16,C.c_uint32,C.c_bool,C.POINTER(C.c_int32),C.POINTER(C.c_uint32)];lib.m1_climate_convert.restype=C.c_bool
        rng=random.Random(20261024);nc=0
        for sequence in range(8):
            u.mem_write(0x200001f8,bytes(128));model=Climate()
            for step in range(100):
                # Hold sensor auxiliary word stable within a device sequence.
                inputs[:]=[rng.randrange(18000,32000),rng.randrange(10000,50000),0x12345678+sequence*12345]
                on=bool((step//17)%2);u.mem_write(0x20000018,bytes((on,)))
                run(0x58c4,0x20007000,0x20007004)
                want=struct.unpack('<iI',u.mem_read(0x20007000,8))
                t=C.c_int32();rh=C.c_uint32();ok=lib.m1_climate_convert(C.byref(model),*inputs,on,C.byref(t),C.byref(rh))
                stock=struct.unpack('<16q',u.mem_read(0x200001f8,128))
                assert list(model.x)==list(stock[6:10]),('state',sequence,step,list(model.x),stock[6:10])
                assert (model.heat,model.aux)==(stock[10],stock[5]),('heat',step,model.heat,model.aux,stock[10],stock[5])
                assert ok and (t.value,rh.value)==want,('conversion',sequence,step,ok,(t.value,rh.value),want,stock[11:])
                nc+=1
        for rt in (0,1,10000,26000,40000,65535):
            for raw_h in (0,1,32768,65535):
                for on in (False,True):
                    for auxiliary in (0,1,65535):
                        u.mem_write(0x200001f8,bytes(128));model=Climate()
                        inputs[:]=[rt,raw_h,auxiliary];u.mem_write(0x20000018,bytes((on,)))
                        run(0x58c4,0x20007000,0x20007004)
                        want=struct.unpack('<iI',u.mem_read(0x20007000,8))
                        t=C.c_int32();rh=C.c_uint32()
                        assert lib.m1_climate_convert(C.byref(model),*inputs,on,C.byref(t),C.byref(rh))
                        assert (t.value,rh.value)==want,('cold boundary',inputs,on,(t.value,rh.value),want)
                        nc+=1
        nd=0
        for i in range(200):
            s=State();lib.m1_init(C.byref(s));s.clock=Clock(2000+rng.randrange(100),rng.randrange(1,13),rng.randrange(1,29),rng.randrange(24),rng.randrange(60),0)
            s.sample=Sample(rng.choice([0,10,11,100,301,9999,10000]),rng.choice([0,1,9,10,75,76,150,151,999,1000]),rng.choice([-1500,-999,-550,-1,0,999,2550,9999]),rng.choice([0,1,999,4567,9999,10000]),1,1,1);s.raw=s.sample.hcho
            u.mem_write(DISPLAY,bytes(29))
            for a,v in [(0x200001dc,s.clock.year-2000),(0x200001d4,s.clock.month),(0x200001d8,s.clock.day),(0x200001db,s.clock.hour),(0x200001df,s.clock.minute)]:u.mem_write(a,bytes((v,)))
            u.mem_write(0x2000408c,struct.pack('<I',s.raw));u.mem_write(0x2000404c,struct.pack('<I',s.sample.pm25))
            u.mem_write(0x20000278,b'\x01')
            t=abs(s.sample.temperature);u.mem_write(0x20004054,bytes((s.sample.temperature<0,t//100,t%100)))
            u.mem_write(0x20004090,bytes((s.sample.humidity//100,s.sample.humidity%100)))
            for at in (0x66dc,0x68b8,0x6ad8,0x6dac,0x6f3c,0x705c):run(at)
            if s.raw<11:s.sample.hcho=10
            out=(C.c_uint8*29)();lib.m1_display_render(C.byref(s),out)
            want=bytes(u.mem_read(DISPLAY,29));got=bytes(out)
            assert got==want,('display',i,[(hex(k),hex(a),hex(b)) for k,(a,b) in enumerate(zip(got,want)) if a!=b],s.raw,s.sample.pm25,s.sample.temperature,s.sample.humidity)
            nd+=1
    return {'status':'PASS','stock_sha256':STOCK_SHA256,'climate_state_and_output_cases':nc,'complete_display_cases':nd,'limits':'emulated stock arithmetic and rendering; no peripheral or hardware proof'}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('image',type=Path);a=p.parse_args();print(json.dumps(verify(a.image.read_bytes()),indent=2))
