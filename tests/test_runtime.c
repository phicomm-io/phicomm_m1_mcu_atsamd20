/* SPDX-License-Identifier: MIT */
#include "m1/runtime.h"
#include "m1/display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned commands[3][256],rtc_writes,resets,display_writes,buzz_changes;
    uint8_t last[3][32],screen[29];size_t last_size[3];
    bool bus_fail,tx_fail,rtc_ok,buzz,status_crc_bad,pwm_fail,wifi,reset_nack;
    uint16_t status,pwm_hz; uint8_t pwm_duty; unsigned status_reads,soft_resets,reset_attempts;
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
        if(nr==3) {
            v->status_reads++;rx[0]=(uint8_t)(v->status>>8);rx[1]=(uint8_t)v->status;
            rx[2]=m1_sht_crc(rx,2);if(v->status_crc_bad) rx[2]^=1;return true;
        }
        assert(nr==6);rx[0]=0x66;rx[1]=0x66;rx[2]=m1_sht_crc(rx,2);
        rx[3]=0x80;rx[4]=0;rx[5]=m1_sht_crc(rx+3,2);
    } else {
        assert(nt==2 && ((tx[0]==0x24 && tx[1]==0)||(tx[0]==0x36 && tx[1]==0x82)||
               (tx[0]==0xf3 && tx[1]==0x2d)||(tx[0]==0x30 && tx[1]==0xa2)));
        if(tx[0]==0x30) { v->reset_attempts++;if(v->reset_nack) return false;v->soft_resets++;v->status=0x10; }
    } }
    return true;
}
static bool rtc_get(void *ctx,m1_clock *c) { fake *v=ctx;if(!v->rtc_ok) return false;*c=v->clock;return true; }
static bool rtc_set(void *ctx,const m1_clock *c) { fake *v=ctx;v->clock=*c;v->rtc_writes++;return true; }
static void buzz(void *ctx,bool on) { fake *v=ctx;v->buzz=on;v->buzz_changes++; }
static bool pwm(void *ctx,uint16_t hz,uint8_t duty)
{ fake *v=ctx;if(v->pwm_fail) return false;v->pwm_hz=hz;v->pwm_duty=duty;return true; }
static void wifi(void *ctx,bool enabled) { ((fake *)ctx)->wifi=enabled; }
static void reset(void *ctx) { ((fake *)ctx)->resets++; }
static void init(uint32_t now)
{
    f=(fake){0};m1_io io={.context=&f,.uart=uart,.i2c=i2c,.rtc_read=rtc_get,.rtc_write=rtc_set,.buzzer=buzz,.reset=reset,.buzzer_pwm=pwm,.wifi_enable=wifi};
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
    tick(1,false);tick(21,false);tick(23,false);assert(r.state.sample.climate_health==1);
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
    init(UINT32_MAX-10);tick(UINT32_MAX-10,false);tick(UINT32_MAX-9,false);tick(10,false);tick(12,false);assert(r.state.sample.climate_health==1);
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
static void pm_modes(void)
{
    init(0);tick(0,false);assert(r.pm_probing);
    const uint8_t ack[8]={0x42,0x4d,0,4,0xe1,0,1,0x74};
    bytes(M1_PM,ack,3,1);bytes(M1_PM,ack+3,5,2);
    assert(r.pm_mode==2 && !r.pm_probing);
    for(unsigned i=0;i<4;++i) { pm(30,3+i);assert(!r.state.sample.pm25); }
    pm(30,7);assert(r.state.sample.pm25==30);
    pm(1200,8);assert(r.state.sample.pm25==1200); /* no mode-3 spike hold */
    m1_runtime_pm_mode(&r,3,9);assert(!r.pm_auto && r.pm_warmup==0);
    pm(10,10);pm(10,11);pm(10,12);pm(1200,13);assert(r.state.sample.pm25==10);
    /* A false header followed by a real frame must retain the overlapping
     * candidate, even when received in several UART chunks. */
    uint8_t noise[11]={0x42,0x4d,0,28};bytes(M1_PM,noise,11,14);pm(1201,15);
    assert(r.state.sample.pm25==1201);
    m1_runtime_pm_mode(&r,0,16);tick(16,false);tick(1016,false);
    assert(!r.pm_probing && r.pm_mode==3);
    tick(10016,false);assert(r.pm_probing);
    bytes(M1_PM,ack,8,10017);assert(r.pm_mode==2 && r.pm_warmup==0);
    m1_runtime_pm_mode(&r,3,10018);tick(10018,false);
    bytes(M1_PM,ack,8,10019);assert(r.pm_mode==3);
}
static void climate_recovery(void)
{
    init(0);f.status=0x8010;tick(0,false);tick(1,false);tick(21,false);tick(23,false);
    assert(r.climate_status==0x8010 && r.climate_status_valid && r.state.sample.climate_health);
    assert(!f.soft_resets); /* reset-detected and alert alone are informational */
    f.status_crc_bad=true;
    tick(1000,false);tick(1001,false);tick(2001,false);tick(2002,false);tick(3002,false);tick(3003,false);
    assert(r.health[2].fault && r.climate_reset_due && !r.climate_status_valid);
    tick(4003,false);assert(f.soft_resets==1 && r.climate_phase==5);
    unsigned reads=f.status_reads;tick(4004,false);assert(f.status_reads==reads);
    f.status_crc_bad=false;tick(4005,false);tick(4006,false);tick(4007,false);tick(4027,false);tick(4029,false);
    assert(r.state.sample.climate_health==1 && !r.health[2].fault && r.climate_resets==1);
    f.status=3;
    for(uint32_t t=5006;t<=14002;++t) tick(t,false);
    assert(f.soft_resets==1 && !r.state.sample.climate_health);
    for(uint32_t t=14003;t<=15050;++t) tick(t,false);
    assert(f.soft_resets==2);
    init(0);f.status_crc_bad=true;f.reset_nack=true;
    for(uint32_t t=0;t<=13000;++t) tick(t,false);
    assert(f.reset_attempts==1 && !f.soft_resets && !r.state.sample.climate_health);
    for(uint32_t t=13001;t<=14010;++t) tick(t,false);
    assert(f.reset_attempts==2 && !f.soft_resets);
    init(UINT32_MAX-100);f.bus_fail=true;
    tick(UINT32_MAX-100,false);tick(899,false);tick(1899,false);f.bus_fail=false;tick(2899,false);
    assert(f.soft_resets==1);
}
static void debug(uint8_t op,const uint8_t args[7],uint32_t at)
{
    uint8_t p[12]={0x23,0x70,op,0x5a,0,0,0,0,0,0,0,0x21};
    if(args) memcpy(p+4,args,7);
    bytes(M1_LINK,p,12,at);
    assert(f.last[M1_LINK][1]==0x70 && f.last[M1_LINK][2]==op && f.last[M1_LINK][3]==0x5a);
}
static void debug_tests(void)
{
    init(0);debug(0,0,0);assert(f.last[M1_LINK][4]==0 && f.last[M1_LINK][5]==1);
    uint8_t tone[7]={0x0b,0xb8,25,0x01,0xf4,0,0};
    debug(1,tone,1);assert(r.debug_buzzer && f.buzz && f.pwm_hz==3000 && f.pwm_duty==25);
    tick(500,false);assert(f.buzz);tick(501,false);assert(!f.buzz && f.pwm_hz==2000 && f.pwm_duty==50);
    tone[2]=101;debug(1,tone,502);assert(f.last[M1_LINK][4]==1 && !r.debug_buzzer);
    tone[2]=50;f.pwm_fail=true;debug(1,tone,503);assert(f.last[M1_LINK][4]==4 && !r.debug_buzzer);f.pwm_fail=false;
    hcho(42,510,false);pm(20,510);pm(20,511);pm(20,512);
    uint8_t sample[7]={0x01,0x2d,0,151,0xfd,0xda,55};
    debug(2,sample,513);tick(513,false);
    assert(r.debug_display && r.debug_state.sample.temperature_centi_c==-550 && f.screen[16]==2);
    cmd(1,0,0,514);assert(f.last[M1_LINK][9]==0 && f.last[M1_LINK][10]==20);
    assert(r.state.hcho_raw==42 && r.state.sample.pm25==20);
    uint8_t raw[7]={0x13,1,0x70,0,0,0,0};debug(4,raw,515);tick(515,false);assert(f.screen[0x13]==0x70);
    uint8_t get[7]={0x13,1,0,0,0,0,0};debug(8,get,516);assert(f.last[M1_LINK][7]==0x70);
    raw[0]=28;raw[1]=2;debug(4,raw,517);assert(f.last[M1_LINK][4]==1 && r.debug_ram[0x13]==0x70);
    uint8_t control[7]={25,1,1,0,50,0,0};debug(3,control,518);tick(518,false);
    for(unsigned i=0;i<29;++i) assert(f.screen[i]==255);
    tick(568,false);assert(!r.debug_display && f.screen[16]!=255 && r.state.brightness==100);
    debug(6,0,569);assert(f.last[M1_LINK][4]==0 && f.last[M1_LINK][5]==3);
    uint8_t mode[7]={2,0,0,0,0,0,0};debug(7,mode,570);assert(r.pm_mode==2);
    debug(255,0,571);assert(f.last[M1_LINK][4]==3);
    debug(2,sample,572);debug(1,tone,573);cmd(0x12,1,0,574);
    assert(r.update.active && !r.debug_display && !r.debug_buzzer && !f.buzz);
    debug(2,sample,575);assert(f.last[M1_LINK][4]==2 && !r.debug_display);
    init(UINT32_MAX-10);tone[3]=0;tone[4]=20;
    debug(1,tone,UINT32_MAX-10);tick(8,false);assert(f.buzz);tick(9,false);assert(!f.buzz);
}
static void debug_stream_fuzz(void)
{
    init(0);uint32_t seed=20261024;
    for(unsigned i=0;i<12000;++i) {
        uint8_t p[12]={0x23,0x70,0,0,0,0,0,0,0,0,0,0x21};
        for(unsigned j=2;j<11;++j) { seed=seed*1664525u+1013904223u;p[j]=(uint8_t)(seed>>24); }
        p[2]=(uint8_t)(i%9);bytes(M1_LINK,p,12,i);tick(i,false);
        assert(r.rx[M1_LINK].used<12 && r.rx[M1_PM].used<32);
    }
    m1_runtime_rx_error(&r,(m1_channel)-1);m1_runtime_rx(&r,(m1_channel)-1,0,0);
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
    clock_tests();button_tests();sensors();protocol();display();pm_modes();climate_recovery();debug_tests();debug_stream_fuzz();arithmetic_fuzz();
    puts("runtime: PM dual modes/SHT recovery/PA24/debug/protocol/button/clock/display/buzzer/wraparound PASS");
    puts("climate: 50000 bounded random conversions PASS");
}
