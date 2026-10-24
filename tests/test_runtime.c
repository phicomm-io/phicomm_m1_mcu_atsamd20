/* SPDX-License-Identifier: MIT */
#include "m1/runtime.h"
#include "m1/display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned commands[3][256],rtc_writes,resets,display_writes,buzz_changes;
    uint8_t last[3][32],screen[29];size_t last_size[3];
    bool bus_fail,tx_fail,rtc_ok,buzz;
    m1_clock clock;
} fake;
static fake f;static m1_runtime r;
static bool uart(void *ctx,m1_channel c,const uint8_t *p,size_t n)
{
    fake *v=ctx;assert(n<=32 && n>=2);v->commands[c][p[1]]++;
    memcpy(v->last[c],p,n);v->last_size[c]=n;return !v->tx_fail;
}
static bool i2c(void *ctx,unsigned bus,uint8_t addr,const uint8_t *tx,size_t nt,uint8_t *rx,size_t nr)
{
    fake *v=ctx;if(v->bus_fail) return false;
    if(bus==0) { assert(addr==0x72 && !nr);if(nt==30) { memcpy(v->screen,tx+1,29);v->display_writes++; } }
    else { assert(bus==2 && addr==0x44);if(nr) {
        assert(nr==6);rx[0]=0x66;rx[1]=0x66;rx[2]=m1_sht_crc(rx,2);
        rx[3]=0x80;rx[4]=0;rx[5]=m1_sht_crc(rx+3,2);
    } else assert(nt==2 && ((tx[0]==0x24 && tx[1]==0)||(tx[0]==0x36 && tx[1]==0x82))); }
    return true;
}
static bool rtc_get(void *ctx,m1_clock *c) { fake *v=ctx;if(!v->rtc_ok) return false;*c=v->clock;return true; }
static bool rtc_set(void *ctx,const m1_clock *c) { fake *v=ctx;v->clock=*c;v->rtc_writes++;return true; }
static void buzz(void *ctx,bool on) { fake *v=ctx;v->buzz=on;v->buzz_changes++; }
static void reset(void *ctx) { ((fake *)ctx)->resets++; }
static void init(uint32_t now)
{
    f=(fake){0};m1_io io={.context=&f,.uart=uart,.i2c=i2c,.rtc_read=rtc_get,.rtc_write=rtc_set,.buzzer=buzz,.reset=reset};
    m1_runtime_init(&r,&io,now);assert(f.commands[M1_LINK][0x17]==1);
}
static void bytes(m1_channel c,const uint8_t *p,size_t n,uint32_t now)
{ for(size_t i=0;i<n;++i) m1_runtime_rx(&r,c,p[i],now); }
static void cmd(uint8_t code,uint8_t a,uint8_t b,uint32_t now)
{ uint8_t p[12]={0x23,code,a,b,0,0,0,0,0,0,0,0x21};bytes(M1_LINK,p,12,now); }
static void pm(uint16_t value,uint32_t now)
{
    uint8_t p[32]={0x42,0x4d,0,28};p[12]=(uint8_t)(value>>8);p[13]=(uint8_t)value;unsigned sum=0;
    for(unsigned i=0;i<30;++i) sum+=p[i];
    p[30]=(uint8_t)(sum>>8);p[31]=(uint8_t)sum;bytes(M1_PM,p,32,now);
}
static void hcho(uint16_t value,uint32_t now,bool corrupt)
{
    uint8_t p[9]={255,0x86,(uint8_t)(value>>8),(uint8_t)value,0,0,0,0,0};unsigned sum=0;
    for(unsigned i=1;i<8;++i) sum+=p[i];
    p[8]=(uint8_t)-sum;if(corrupt) p[8]^=1;bytes(M1_HCHO,p,9,now);
}
static void tick(uint32_t at,bool pressed) { m1_runtime_poll(&r,at,pressed); }
static void clock_tests(void)
{
    m1_clock c={2024,2,28,23,59,59};m1_clock_second(&c);assert(c.day==29 && !c.hour && !c.second);
    c=(m1_clock){2025,2,28,23,59,59};m1_clock_second(&c);assert(c.month==3 && c.day==1);
    c=(m1_clock){2099,12,31,23,59,59};m1_clock_second(&c);assert(c.year==2000 && c.month==1 && c.day==1);
    uint8_t reg[8];m1_clock check;assert(m1_rtc_encode(&c,reg));assert(m1_rtc_decode(reg,&check));assert(check.year==c.year);
    reg[0]=0x80;assert(!m1_rtc_decode(reg,&check));reg[0]=0x6a;assert(!m1_rtc_decode(reg,&check));
    c.year=2100;assert(!m1_rtc_encode(&c,reg));
}
static void button_tests(void)
{
    init(0);tick(0,false);tick(100,true);tick(110,false);tick(130,false);assert(r.state.display_on);
    tick(200,true);tick(220,true);tick(400,false);tick(420,false);
    assert(!r.state.display_on && f.commands[M1_LINK][0x0f]==1);
    tick(1000,true);tick(1020,true);tick(4010,true);tick(4011,true);
    assert(f.commands[M1_LINK][4]==1);tick(11000,true);tick(11001,true);
    assert(r.state.display_test && f.commands[M1_LINK][0x10]==1);
    tick(11100,false);tick(11120,false);assert(f.commands[M1_LINK][0x18]==1);
    init(UINT32_MAX-300);tick(UINT32_MAX-200,true);tick(UINT32_MAX-180,true);tick(100,false);tick(120,false);
    assert(!r.state.display_on && f.commands[M1_LINK][0x0f]==1);
}
static void sensors(void)
{
    init(0);tick(0,false);assert(f.commands[M1_HCHO][1]==1 && f.commands[M1_PM][0x4d]==1);
    tick(20,false);tick(22,false);assert(r.state.sample.climate_health==1);
    hcho(155,30,false);assert(r.state.sample.hcho_health==1 && r.state.hcho_raw==155);
    cmd(0x19,0,0,31);assert(f.last[M1_LINK][1]==0x19 && f.last[M1_LINK][2]==0 && f.last[M1_LINK][3]==155);
    tick(32,false);assert(r.state.sample.hcho==155);
    hcho(0,40,false);tick(40,false);assert(r.state.sample.hcho==10);
    pm(30,50);assert(r.state.sample.pm25==0);pm(30,51);assert(r.state.sample.pm25==0);
    pm(30,52);assert(r.state.sample.pm25==30);pm(1200,53);assert(r.state.sample.pm25==30);
    pm(1201,54);assert(r.state.sample.pm25==1201);pm(20,55);assert(r.state.sample.pm25==20);
    for(unsigned i=0;i<3;++i) hcho(42,100+i,true);
    tick(103,false);assert(r.state.sample.hcho_health==0 && f.commands[M1_LINK][10]==1);
    hcho(42,104,false);assert(r.state.sample.hcho_health==1);
    tick(6200,false);assert(!r.state.sample.hcho_health && !r.state.sample.pm25_health);
    assert(f.commands[M1_LINK][8]==1 && f.commands[M1_LINK][9]==1);
    hcho(23,6201,false);assert(r.state.sample.hcho_health==1);
    m1_runtime_rx_error(&r,M1_HCHO);assert(r.rx_errors>0);
    init(0);f.bus_fail=true;tick(0,false);assert(r.display_errors==1 && !r.display_ready);
    tick(100,false);assert(r.display_errors==1);f.bus_fail=false;tick(1000,false);assert(r.display_ready);
    init(UINT32_MAX-10);tick(UINT32_MAX-10,false);tick(9,false);tick(11,false);assert(r.state.sample.climate_health==1);
}
static void protocol(void)
{
    init(0);m1_runtime_beep(&r,M1_BUZZER_CHIRP,0);tick(0,false);assert(f.buzz);
    tick(199,false);assert(f.buzz);tick(200,false);assert(!f.buzz);
    init(0);tick(600000,false);unsigned minute=r.state.clock.minute;
    tick(601000,false);assert(r.state.clock.minute==minute && r.state.clock.second==1);
    init(0);uint8_t a[12]={0x23,0x11,0,0,0,0,0,0,0,0,0,0x21};
    bytes(M1_LINK,a,6,0);bytes(M1_LINK,a+6,6,101);assert(f.commands[M1_LINK][0x11]==0);
    bytes(M1_LINK,a,12,102);assert(f.commands[M1_LINK][0x11]==1);
    cmd(2,25,0,103);tick(103,false);assert(!r.state.display_on && r.state.brightness==25);
    cmd(2,25,2,104);assert(r.state.display_on);cmd(2,26,1,105);assert(r.state.brightness==25 && f.commands[M1_LINK][7]==1);
    cmd(0x0e,1,0,106);tick(106,false);assert(f.buzz);cmd(0x0e,0,0,107);tick(107,false);assert(!f.buzz);
    hcho(301,108,false);tick(108,false);assert(f.buzz && r.buzzer_mode==3);
    tick(308,false);assert(!f.buzz);tick(708,false);assert(f.buzz);tick(6508,false);assert(!f.buzz);
    uint8_t time[12]={0x23,3,0x07,0xea,10,24,12,34,56,0,0,0x21};bytes(M1_LINK,time,12,7000);
    assert(f.rtc_writes==1 && f.clock.year==2026 && f.clock.second==56);f.rtc_ok=true;f.clock.second=57;
    tick(8000,false);assert(r.state.clock.second==57 && f.commands[M1_LINK][0x0c]>0);
    assert(m1_update_begin(&r.update,1,0,9000));unsigned notifications=f.commands[M1_LINK][0x0c];
    tick(9000,false);assert(f.commands[M1_LINK][0x0c]==notifications);
    tick(39001,false);assert(!r.update.active);f.tx_fail=true;cmd(0x11,0,0,39002);assert(r.tx_errors>0);
}
static void display(void)
{
    m1_state s;uint8_t ram[29];m1_init(&s);s.clock=(m1_clock){2026,10,24,12,34,0};
    const uint16_t hs[]={100,101,300,301},ps[]={75,76,150,151};
    const unsigned groups[]={M1_GROUP_GREEN,M1_GROUP_YELLOW,M1_GROUP_YELLOW,M1_GROUP_RED};
    const unsigned offset[]={0,0x0c,0x13,0x12};
    for(unsigned h=0;h<4;++h) for(unsigned p=0;p<4;++p) {
        s.hcho_raw=hs[h];s.sample.hcho=hs[h];s.sample.pm25=ps[p];m1_display_render(&s,ram);
        for(unsigned g=1;g<=3;++g) {
            assert((ram[offset[g]]&0x70)==(groups[h]==g?0x70:0));
            assert((ram[offset[g]]&0x85)==(groups[p]==g?0x85:0));
        }
    }
    s.sample.temperature_centi_c=-550;m1_display_render(&s,ram);assert(ram[16]==2);
    s.display_test=true;m1_display_render(&s,ram);for(unsigned i=0;i<29;++i) assert(ram[i]==255);
    assert(m1_display_brightness(25)==0xb1 && m1_display_brightness(50)==0xb7 && m1_display_brightness(75)==0xbb);
}
static void arithmetic_fuzz(void)
{
    uint32_t seed=20261024;
    for(unsigned trial=0;trial<100;++trial) {
        m1_climate c={0};
        for(unsigned i=0;i<500;++i) {
            seed=seed*1664525u+1013904223u;uint16_t t=(uint16_t)(seed>>16);
            seed=seed*1664525u+1013904223u;uint16_t h=(uint16_t)(seed>>16);
            m1_climate next=c;int32_t tm;uint32_t rh;
            if(m1_climate_convert(&next,t,h,seed,(i&1)!=0,&tm,&rh)) { assert(tm>=-50000 && tm<=130000 && rh<=100000);c=next; }
        }
    }
}
int main(void)
{
    clock_tests();button_tests();sensors();protocol();display();arithmetic_fuzz();
    puts("runtime: sensors/recovery/protocol/button/clock/display/buzzer/wraparound PASS");
    puts("climate: 50000 bounded random conversions PASS");
}
