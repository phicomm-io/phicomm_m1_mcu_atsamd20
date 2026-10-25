/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/runtime.h"
#include "m1/display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned commands[3][256],rtc_writes,resets,display_writes,buzz_changes;
    uint32_t display_times[64],now;
    uint8_t last[3][32],screen[29];size_t last_size[3];
    bool bus_fail,tx_fail,rtc_ok,buzz,status_crc_bad,pwm_fail,wifi,reset_nack,aux_crc_bad;
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
    if(bus==0) { assert(addr==0x72 && !nr);if(nt==30) {
        memcpy(v->screen,tx+1,29);
        if(v->display_writes<64) v->display_times[v->display_writes]=v->now;
        v->display_writes++;
    } }
    else { assert(bus==2 && addr==0x44);if(nr) {
        if(nr==3) {
            v->status_reads++;rx[0]=(uint8_t)(v->status>>8);rx[1]=(uint8_t)v->status;
            rx[2]=m1_sht_crc(rx,2);if(v->status_crc_bad) rx[2]^=1;return true;
        }
        assert(nr==6);rx[0]=0x66;rx[1]=0x66;rx[2]=m1_sht_crc(rx,2);
        rx[3]=0x80;rx[4]=0;rx[5]=m1_sht_crc(rx+3,2);
        if(v->aux_crc_bad && r.climate_phase==2)rx[5]^=1;
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
static void pm_frame_error(uint16_t value,uint8_t error_code,uint32_t now)
{
    uint8_t p[32]={0x42,0x4d,0,28};p[12]=(uint8_t)(value>>8);p[13]=(uint8_t)value;unsigned sum=0;
    p[29]=error_code;
    for(unsigned i=0;i<30;++i) sum+=p[i];
    p[30]=(uint8_t)(sum>>8);p[31]=(uint8_t)sum;bytes(M1_PM,p,32,now);
}
static void pm_frame(uint16_t value,uint32_t now)
{ pm_frame_error(value,0,now); }
static void pm(uint16_t value,uint32_t now)
{
    /* Isolated callback/filter cases explicitly arm a complete stock-sized
     * receive job; pm_cadence() separately exercises the real poll schedule. */
    r.pm_armed=true;pm_frame(value,now);
    while(r.pm_armed) m1_runtime_rx(&r,M1_PM,0,now);
}
static void hcho(uint16_t value,uint32_t now,bool corrupt)
{
    uint8_t p[9]={255,0x86,(uint8_t)(value>>8),(uint8_t)value,0,0,0,0,0};unsigned sum=0;
    for(unsigned i=1;i<8;++i) sum+=p[i];
    p[8]=(uint8_t)-sum;if(corrupt) p[8]^=1;bytes(M1_HCHO,p,9,now);
}
static void tick(uint32_t at,bool pressed) { f.now=at;m1_runtime_poll(&r,at,pressed); }
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
    assert(r.state.display_on && !f.commands[M1_LINK][0x0f]);tick(771,false);
    assert(!r.state.display_on && f.commands[M1_LINK][0x0f]==1);
    tick(1000,true);tick(1020,true);tick(4010,true);tick(4011,true);
    assert(f.commands[M1_LINK][4]==1);tick(11000,true);tick(11001,true);
    assert(r.state.display_test && f.commands[M1_LINK][0x10]==1);
    tick(11100,false);tick(11120,false);assert(f.commands[M1_LINK][0x18]==1);
    init(UINT32_MAX-300);tick(UINT32_MAX-200,true);tick(UINT32_MAX-180,true);tick(100,false);tick(120,false);
    tick(471,false);
    assert(!r.state.display_on && f.commands[M1_LINK][0x0f]==1);
    init(0);tick(100,true);tick(120,true);tick(200,false);tick(220,false);
    tick(300,true);tick(320,true);tick(400,false);tick(420,false);tick(800,false);
    assert(r.state.display_on && !f.commands[M1_LINK][0x0f]);
    assert(f.commands[M1_LINK][0x71]==1 && f.last[M1_LINK][2]==1 && f.last[M1_LINK][3]==2);
    /* A short bounce cannot become a second click. */
    init(0);tick(100,true);tick(120,true);tick(200,false);tick(220,false);
    tick(250,true);tick(260,false);tick(280,false);tick(571,false);
    assert(!r.state.display_on && f.commands[M1_LINK][0x0f]==1);
    init(UINT32_MAX-500);tick(UINT32_MAX-400,true);tick(UINT32_MAX-380,true);
    tick(UINT32_MAX-300,false);tick(UINT32_MAX-280,false);tick(UINT32_MAX-200,true);tick(UINT32_MAX-180,true);
    tick(0,false);tick(20,false);assert(f.commands[M1_LINK][0x71]==1 && r.state.display_on);
    /* OTA cancels a pending click and consumes physical transitions. */
    init(0);tick(100,true);tick(120,true);tick(200,false);tick(220,false);
    r.update.active=true;tick(600,false);assert(!f.commands[M1_LINK][0x0f]&&!r.button_click_pending);
}
static void sensors(void)
{
    init(0);tick(0,false);assert(f.commands[M1_HCHO][1]==1 && f.commands[M1_PM][0x4d]==1);
    tick(1,false);tick(21,false);tick(23,false);assert(r.state.sample.climate_health==1);
    hcho(155,30,false);assert(r.state.sample.hcho_health==1 && r.state.hcho_raw==155);
    cmd(0x19,0,0,31);assert(f.last[M1_LINK][1]==0x19 && f.last[M1_LINK][2]==0 && f.last[M1_LINK][3]==155);
    tick(32,false);assert(r.state.sample.hcho==0); /* reply remains at render snapshot */
    hcho(0,40,false);tick(40,false);assert(r.state.sample.hcho==0);
    pm(30,50);assert(r.measured.pm25==0);pm(30,51);assert(r.measured.pm25==0);
    pm(30,52);assert(r.measured.pm25==30);pm(1200,53);assert(r.measured.pm25==30);
    pm(1201,54);assert(r.measured.pm25>=900 && r.measured.pm25<1201);
    pm(20,55);assert(r.measured.pm25>=900); /* one dip cannot clear pollution */
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
    r.sensor_alarm_mask=1;
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
    for(unsigned i=0;i<4;++i) { pm(30,3+i);assert(!r.measured.pm25); }
    pm(30,7);assert(r.measured.pm25==30);
    pm(1200,8);assert(r.measured.pm25==30); /* both modes reject isolated spikes */
    m1_runtime_pm_mode(&r,3,9);assert(!r.pm_auto && r.pm_warmup==0);
    pm(10,10);pm(10,11);pm(10,12);pm(1200,13);assert(r.measured.pm25==10);
    /* A false header followed by a real frame must retain the overlapping
     * candidate, even when received in several UART chunks. */
    uint8_t noise[11]={0x42,0x4d,0,28};r.pm_armed=true;bytes(M1_PM,noise,11,14);pm(1201,15);
    assert(r.measured.pm25>=900 && r.measured.pm25<1201);
    m1_runtime_pm_mode(&r,0,16);tick(16,false);tick(1016,false);
    assert(!r.pm_probing && r.pm_mode==3);
    tick(10016,false);assert(r.pm_probing);
    bytes(M1_PM,ack,8,10017);assert(r.pm_mode==2 && r.pm_warmup==0);
    m1_runtime_pm_mode(&r,3,10018);tick(10018,false);
    bytes(M1_PM,ack,8,10019);assert(r.pm_mode==3);
}
static void sensor_cadence(void)
{
    const uint32_t origins[]={0,UINT32_MAX-1000};
    for(unsigned origin=0;origin<2;++origin) {
        uint32_t start=origins[origin];init(start);tick(start,false);
        /* Isolate publication from acquisition, then vary every millisecond.
         * Normal sampling/compensation is exercised in sensors()/recovery(). */
        r.climate_at=start+60000;
        uint8_t initial[29];memcpy(initial,f.screen,29);
        for(uint32_t ms=1;ms<=13000;++ms) {
            hcho((uint16_t)(150+ms%997),start+ms,false);
            r.measured.pm25=(uint16_t)(ms%211);
            r.measured.temperature_centi_c=(int16_t)(-550+ms%100);
            r.measured.humidity_centi_pct=(uint16_t)(5500+ms%79);
            tick(start+ms,false);
            uint32_t snapshot=ms/3000*3000;
            assert(r.state.sample.hcho==(snapshot?150+snapshot%997:0));
            assert(r.state.sample.pm25==(snapshot?snapshot%211:0));
            assert(r.state.sample.temperature_centi_c==(snapshot?-550+(int32_t)(snapshot%100):0));
            assert(r.state.sample.humidity_centi_pct==(snapshot?5500+snapshot%79:0));
            if(ms<2200) for(unsigned i=0;i<29;++i)
                assert((f.screen[i]&(i==0 || i==3?0xf7:0xff))==
                       (initial[i]&(i==0 || i==3?0xf7:0xff)));
            if(ms==1000 || ms==3001) {
                cmd(1,0,0,start+ms);
                assert(((unsigned)f.last[M1_LINK][2]<<8|f.last[M1_LINK][3])==r.state.sample.hcho);
                assert(((unsigned)f.last[M1_LINK][9]<<8|f.last[M1_LINK][10])==r.state.sample.pm25);
                cmd(0x19,0,0,start+ms);
                assert(((unsigned)f.last[M1_LINK][2]<<8|f.last[M1_LINK][3])==150+ms%997);
            }
        }
        unsigned writes=0;
        for(unsigned ms=0;ms<=13000;++ms)
            if(ms%500==0 || (ms>=2200 && (ms-2200)%2100==0))
                assert(f.display_times[writes++]==start+ms);
        assert(f.display_writes==writes);
    }
    /* Commands, debug, and error recovery cannot publish newer measurements
     * or advance the low-HCHO latch between normal render deadlines. */
    init(0);tick(0,false);hcho(5,1,false);
    cmd(2,25,1,100);tick(100,false);
    assert(r.state.sample.hcho==0 && r.display_at==3000 && r.display_send_at==2200);
    tick(3000,false);assert(r.state.sample.hcho==10);
    hcho(123,3001,false);r.measured.pm25=72;
    cmd(2,50,1,3100);tick(3100,false);assert(r.state.sample.hcho==10 && r.state.sample.pm25==0);
    f.bus_fail=true;tick(4300,false);tick(6000,false);
    assert(r.state.sample.hcho==123 && r.state.sample.pm25==72); /* display failure does not freeze replies */
    init(0);tick(0,false);r.state.display_test=true;hcho(77,100,false);
    tick(3000,false);assert(r.state.sample.hcho==0);
    r.state.display_test=false;tick(6000,false);assert(r.state.sample.hcho==77);
}
static void pm_cadence(void)
{
    init(0);m1_runtime_pm_mode(&r,3,0);tick(0,false);
    pm_frame(30,100);assert(r.rx[M1_PM].used==32 && !r.pm_warmup);
    pm_frame(99,1100);assert(r.pm_warmup==1 && !r.pm_armed && r.measured.pm25==0);
    pm_frame(999,2000);assert(r.rx[M1_PM].used==0 && r.pm_warmup==1);
    tick(2999,false);assert(!r.pm_armed);tick(3000,false);assert(r.pm_armed);
    pm_frame(31,3100);pm_frame(98,4100);assert(r.pm_warmup==2 && !r.measured.pm25);
    tick(6000,false);pm_frame(37,6100);pm_frame(400,7100);
    assert(r.measured.pm25==37 && r.state.sample.pm25==0); /* first aligned candidate, no mean */
    tick(9000,false);assert(r.state.sample.pm25==37);
    /* The original receive job may span scheduler passes; a 100 ms byte-gap
     * timeout or restarting its buffer would prevent a full 64-byte capture. */
    init(0);m1_runtime_pm_mode(&r,3,0);tick(0,false);pm_frame(30,100);
    tick(3000,false);assert(r.rx[M1_PM].used==32);pm_frame(40,3100);assert(r.pm_warmup==1);
    init(0);m1_runtime_pm_mode(&r,2,0);tick(0,false);
    for(unsigned i=0;i<5;++i) {
        if(i) tick(i*3000,false);
        pm_frame(77,i*3000+100);
        assert(r.measured.pm25==(i==4?77:0) && !r.pm_armed);
        pm_frame(333,i*3000+200);assert(!r.pm_armed); /* ignored until next request */
    }
    assert(f.commands[M1_PM][0x4d]==5 && r.pm_warmup==4);
    tick(15000,false);assert(r.state.sample.pm25==77);
    /* Mode 3 counts corrupt windows for warm-up, and searches offsets 0..31.
     * A frame beginning at byte 32 belongs to the next half, not a candidate. */
    init(0);m1_runtime_pm_mode(&r,3,0);tick(0,false);
    for(unsigned window=0;window<2;++window) {
        if(window) tick(3000,false);
        for(unsigned i=0;i<64;++i) m1_runtime_rx(&r,M1_PM,0,window*3000+100);
    }
    assert(r.pm_warmup==2 && !r.measured.pm25);
    tick(6000,false);
    for(unsigned i=0;i<31;++i) m1_runtime_rx(&r,M1_PM,0,6100);
    pm_frame(42,6100);m1_runtime_rx(&r,M1_PM,0,6100);
    assert(!r.pm_armed && r.measured.pm25==42);
    tick(9000,false);
    for(unsigned i=0;i<32;++i) m1_runtime_rx(&r,M1_PM,0,9100);
    pm_frame(99,9100);assert(r.measured.pm25==42);
    /* ACK recognition must not mistake a valid data-frame prefix for an ACK. */
    init(0);tick(0,false);
    const uint8_t prefix[8]={0x42,0x4d,0,28,0,0,1,0x74};
    bytes(M1_PM,prefix,8,1);assert(r.pm_mode==3 && r.rx[M1_PM].used==8);
}
static void pm_stability(void)
{
    const uint32_t origins[]={0,UINT32_MAX-5000};
    for(unsigned m=2;m<=3;++m) for(unsigned o=0;o<2;++o) {
        uint32_t at=origins[o];init(at);m1_runtime_pm_mode(&r,(uint8_t)m,at);
        unsigned warm=m==2?4:2;
        for(unsigned i=0;i<warm;++i) {
            pm(40,at);assert(!r.measured.pm25 && !r.state.sample.pm25_health);at+=3000;
        }
        pm(40,at);assert(r.measured.pm25==40 && r.state.sample.pm25_health);at+=3000;
        pm(180,at);assert(r.measured.pm25==40);
        assert(r.sensor_frames[M1_SENSOR_PM].flags&M1_FRAME_FILTERED);
        assert(r.sensor_frames[M1_SENSOR_PM].bytes[12]==0 && r.sensor_frames[M1_SENSOR_PM].bytes[13]==180);
        at+=3000;pm(40,at);assert(r.measured.pm25==40);at+=3000;
        pm(0,at);assert(r.measured.pm25==40);at+=3000;
        pm(40,at);assert(r.measured.pm25==40);at+=3000;
        pm(180,at);assert(r.measured.pm25==40);at+=3000;
        pm(180,at);assert(r.measured.pm25>=140 && r.measured.pm25<180);at+=3000;
        pm(180,at);assert(r.measured.pm25>=170 && r.measured.pm25<=180);
        uint16_t last=r.measured.pm25;at+=3000;
        /* A sensor error is visible in raw diagnostics, never as a zero
         * measurement. Three errors fail health immediately. */
        for(unsigned i=0;i<3;++i) {
            r.pm_armed=true;pm_frame_error(7,1,at);
            while(r.pm_armed)m1_runtime_rx(&r,M1_PM,0,at);
            assert(r.measured.pm25==last);
            assert(r.sensor_frames[M1_SENSOR_PM].flags&M1_FRAME_ERROR);at+=100;
        }
        assert(!r.state.sample.pm25_health && !r.pm_filter.seeded);
        for(unsigned i=0;i<warm;++i) {
            pm(8,at);assert(!r.measured.pm25 && !r.state.sample.pm25_health);at+=3000;
        }
        pm(8,at);assert(r.measured.pm25==8 && r.state.sample.pm25_health);at+=3000;
        /* A transport error must not confirm two separated outliers. */
        pm(300,at);assert(r.measured.pm25==8);at+=1000;
        m1_runtime_rx_error(&r,M1_PM);at+=2000;
        pm(300,at);assert(r.measured.pm25==8);at+=3000;
        pm(300,at);assert(r.measured.pm25>200);at+=7000;
        /* Recovery also works if UART data precedes poll after a long stall. */
        pm(55,at);assert(!r.measured.pm25 && !r.state.sample.pm25_health);
        at+=3000;
        for(unsigned i=1;i<warm;++i) { pm(55,at);assert(!r.measured.pm25);at+=3000; }
        pm(55,at);assert(r.measured.pm25==55 && r.state.sample.pm25_health);
    }
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
static void co2_tests(void)
{
    init(0);tick(0,false);uint8_t original[29];memcpy(original,f.screen,29);
    uint8_t cfg[7]={2,0,0,2,0,0,0};debug(10,cfg,10);
    uint8_t value[7]={3,0xe7,1,0,20,0,0};debug(12,value,10);tick(10,false);
    assert(r.co2_show && !(f.screen[10]&8) && !(f.screen[4]&8) && !(f.screen[21]&0x30));
    assert(f.screen[10]==0xd7 && f.screen[9]==0xd7 && f.screen[4]==0xd7);
    assert((f.screen[19]&0x70)==0x70);
    value[0]=3;value[1]=0xe8;debug(12,value,1000);tick(1000,false);
    assert(r.co2_show && (f.screen[10]&8) && !(f.screen[4]&8));
    assert(f.screen[10]==(0x05|8) && f.screen[9]==0xf5 && f.screen[4]==0xf5);
    tick(2010,false);assert(!r.co2_show);assert(!memcmp(f.screen,r.display_ram,29));
    tick(4010,false);assert(r.co2_show); /* updates do not restart alternation */
    debug(13,0,4010);assert(f.last[M1_LINK][15] && f.last[M1_LINK][16]);
    uint8_t pair[7]={2,0x01,0xf4,0x02,0x58,0,0};debug(11,pair,4010);
    assert(r.co2_threshold[0]==500 && r.co2_threshold[1]==600);
    tick(4010,false);assert((f.screen[12]&0x70)==0x70);
    tick(21000,false);assert(!r.co2_show && !r.co2_valid);
    cfg[0]=1;cfg[1]=1;debug(10,cfg,21000);
    value[0]=0x09;value[1]=0xc4;debug(12,value,21000);tick(21000,false);
    assert(r.co2_show && !(f.screen[21]&0x85) && !(f.screen[22]&8));
    assert(f.screen[24]==(0xb3|8) && f.screen[23]==0xd6 && f.screen[22]==0xf5);
    value[2]=0;debug(12,value,21001);tick(21001,false);assert(!r.co2_show);
    cfg[0]=3;debug(10,cfg,22000);assert(f.last[M1_LINK][4]==1 && r.co2_mode==1);
    cfg[0]=2;cfg[2]=cfg[3]=0;debug(10,cfg,22000);assert(f.last[M1_LINK][4]==1);
    init(UINT32_MAX-1000);cfg[0]=1;cfg[3]=2;debug(10,cfg,UINT32_MAX-900);
    value[2]=1;value[4]=2;debug(12,value,UINT32_MAX-900);
    tick(1000,false);assert(r.co2_show);tick(1100,false);assert(!r.co2_show);
}
static void external_display_tests(void)
{
    const uint32_t origins[]={0,UINT32_MAX-1000};
    for(unsigned i=0;i<2;i++){
        uint32_t now=origins[i];init(now);tick(now,false);
        uint8_t value[7]={0,125,1,M1_GROUP_YELLOW,2,0,0},source[7]={1};
        debug(0,0,now);assert(f.last[M1_LINK][16]==1);
        m1_sample measured=r.measured,original=r.state.sample;
        debug(17,value,now);tick(now,false);
        assert(!r.external_source && !memcmp(f.screen,r.display_ram,29));
        debug(18,source,now);tick(now,false);
        assert(f.screen[10]==(0x05|8) && f.screen[9]==0xb3 && f.screen[4]==0xd6);
        assert(!(f.screen[21]&0x30) && !(f.screen[4]&8));
        assert((f.screen[18]&0x70)==0x70);
        /* Only the selected numeric/legend/lamp fields change. */
        for(unsigned j=0;j<29;j++){
            if(j==4||j==9||j==10||j==12||j==18||j==19||j==21)continue;
            assert(f.screen[j]==r.display_ram[j]);
        }
        assert(!memcmp(&measured,&r.measured,sizeof(measured)));
        assert(!memcmp(&original,&r.state.sample,sizeof(original)));
        debug(19,0,now+1);assert(f.last[M1_LINK][5]==1 && f.last[M1_LINK][6]==1 && f.last[M1_LINK][7]==1);
        tick(now+2000,false);assert(!r.external_valid);
        assert((f.screen[10]&~8u)==(r.display_ram[10]&~8u));
        debug(19,0,now+2000);assert(!f.last[M1_LINK][7] && !f.last[M1_LINK][13]);
        /* Renewing data resumes the selected source; no fake zero reading. */
        debug(17,value,now+2100);tick(now+2100,false);assert(r.external_valid);
        value[2]=4;value[3]=0;value[5]=1;debug(17,value,now+2101);tick(now+2101,false);
        assert((f.screen[21]&0x85)==0x85 && (f.screen[22]&8));
        assert(!(f.screen[12]&0x85) && !(f.screen[18]&0x85) && !(f.screen[19]&0x85));
        /* Reject malformed requests without partially applying them. */
        const m1_runtime before=r;
        value[6]=1;debug(17,value,now+2102);assert(f.last[M1_LINK][4]==1);
        assert(r.external_number==before.external_number && r.external_until==before.external_until);
        value[6]=0;value[2]=3;debug(17,value,now+2102);assert(f.last[M1_LINK][4]==1);
        value[2]=0;value[0]=4;debug(17,value,now+2102);assert(f.last[M1_LINK][4]==1);
        value[0]=0;value[4]=61;debug(17,value,now+2102);assert(f.last[M1_LINK][4]==1);
        source[1]=1;debug(18,source,now+2102);assert(f.last[M1_LINK][4]==1 && r.external_source==1);
        source[0]=source[1]=0;debug(18,source,now+2103);tick(now+2103,false);
        assert(!r.external_source && !memcmp(f.screen,r.display_ram,29));
        value[4]=2;debug(17,value,now+2104);source[0]=1;debug(18,source,now+2104);
        uint8_t overlay[7]={100,1,1,0,100};debug(3,overlay,now+2105);tick(now+2105,false);
        for(unsigned j=0;j<29;j++)assert(f.screen[j]==255);
        cmd(0x12,1,0,now+2106);debug(18,source,now+2107);assert(f.last[M1_LINK][4]==2);
    }
}
static void colon_tests(void)
{
    const uint32_t origins[]={0,UINT32_MAX-250};
    for(unsigned i=0;i<2;++i) {
        uint32_t start=origins[i];init(start);tick(start,false);
        uint8_t original[29];memcpy(original,f.screen,29);
        tick(start+499,false);assert((f.screen[0]&8) && (f.screen[3]&8));
        tick(start+500,false);assert(!(f.screen[0]&8) && !(f.screen[3]&8));
        for(unsigned j=0;j<29;++j)
            assert(f.screen[j]==(uint8_t)(original[j] & (j==0 || j==3?0xf7:0xff)));
        uint8_t get[7]={0,4};debug(8,get,start+500);
        assert(!memcmp(f.last[M1_LINK]+7,f.screen,4));
        tick(start+999,false);assert(!(f.screen[0]&8));
        tick(start+1000,false);assert((f.screen[0]&8) && (f.screen[3]&8));
        /* A stalled poll skips missed phases in one transfer. */
        unsigned writes=f.display_writes;tick(start+2750,false);
        assert(f.display_writes==writes+1 && !r.colon_on && r.colon_at==start+3000);
    }
    init(0);tick(0,false);f.bus_fail=true;tick(500,false);
    assert(r.dirty && r.display_errors==1);f.bus_fail=false;
    tick(1000,false);assert(r.display_errors==1);
    tick(1500,false);assert(!r.dirty && !(f.screen[0]&8));
    /* Raw and full-segment diagnostics bypass the blink overlay. */
    uint8_t raw[7]={0,4,0xff,0xff,0xff,0xff};debug(4,raw,1600);tick(1600,false);
    unsigned writes=f.display_writes;tick(2000,false);assert(f.display_writes==writes);
    for(unsigned j=0;j<4;++j)assert(f.screen[j]==0xff);
    uint8_t all[7]={100,1,1,0x13,0x88};debug(3,all,2100);tick(2100,false);tick(2500,false);
    for(unsigned j=0;j<29;++j)assert(f.screen[j]==0xff);
    debug(5,0,2600);tick(2600,false);assert(!(f.screen[0]&8));
    cmd(2,100,0,2700);tick(2700,false);writes=f.display_writes;
    tick(3000,false);assert(f.display_writes==writes);
    cmd(2,100,1,3100);tick(3100,false);assert((f.screen[0]&8) && (f.screen[3]&8));
    /* Explicit time synchronization restarts a complete lit half-cycle. */
    uint8_t time[12]={0x23,3,0x07,0xea,10,25,12,34,56,0,0,0x21};
    bytes(M1_LINK,time,12,3700);tick(3700,false);assert(r.colon_on && r.colon_at==4200);
    tick(4199,false);assert(f.screen[0]&8);tick(4200,false);assert(!(f.screen[0]&8));
}
static void tone_tests(void)
{
    const uint16_t hz[]={0,2000,2500,1500,1000},ms[]={0,60,120,250,500};
    for(unsigned id=1;id<=4;++id) {
        init(0);uint8_t a[7]={(uint8_t)id};debug(14,a,10);
        assert(f.last[M1_LINK][4]==0 && f.last[M1_LINK][5]==id);
        assert(((unsigned)f.last[M1_LINK][6]<<8|f.last[M1_LINK][7])==hz[id]);
        assert(((unsigned)f.last[M1_LINK][8]<<8|f.last[M1_LINK][9])==ms[id]);
        assert(f.last[M1_LINK][10]==50 && r.debug_buzzer && f.buzz);
        assert(f.pwm_hz==hz[id] && f.pwm_duty==50);
        tick(10+ms[id]-1,false);assert(f.buzz);
        tick(10+ms[id],false);assert(!f.buzz && !r.debug_buzzer && f.pwm_hz==2000);
    }
    init(0);uint8_t a[7]={4};debug(14,a,0);
    a[0]=5;debug(14,a,1);assert(f.last[M1_LINK][4]==1 && r.debug_buzzer_until==500 && f.buzz);
    a[0]=1;a[6]=1;debug(14,a,2);assert(f.last[M1_LINK][4]==1 && f.pwm_hz==1000);
    a[6]=0;debug(14,a,100);assert(r.debug_buzzer_until==160 && f.pwm_hz==2000);
    a[0]=0;debug(14,a,101);assert(!f.buzz && !r.debug_buzzer);
    r.io.buzzer_pwm=0;a[0]=2;debug(14,a,102);assert(f.last[M1_LINK][4]==3 && !f.buzz);
    r.io.buzzer_pwm=pwm;f.pwm_fail=true;debug(14,a,103);
    assert(f.last[M1_LINK][4]==4 && !f.buzz && !r.debug_buzzer);
    init(UINT32_MAX-30);a[0]=1;debug(14,a,UINT32_MAX-30);
    tick(28,false);assert(f.buzz);tick(29,false);assert(!f.buzz);
    init(0);a[0]=4;debug(14,a,0);cmd(0x12,1,0,1);
    assert(r.update.active && !r.debug_buzzer && !f.buzz);
    debug(14,a,2);assert(f.last[M1_LINK][4]==2);
    init(0);cmd(0x0e,1,0,0);tick(0,false);a[0]=1;debug(14,a,1);
    tick(61,false);assert(!r.debug_buzzer && f.buzz && f.pwm_hz==2000);
    cmd(0x0e,0,0,62);tick(62,false);assert(!f.buzz);
}
static void debug_tests(void)
{
    co2_tests();init(0);debug(0,0,0);assert(f.last[M1_LINK][4]==0 && f.last[M1_LINK][5]==2);
    uint8_t tone[7]={0x0b,0xb8,25,0x01,0xf4,0,0};
    debug(1,tone,1);assert(r.debug_buzzer && f.buzz && f.pwm_hz==3000 && f.pwm_duty==25);
    tick(500,false);assert(f.buzz);tick(501,false);assert(!f.buzz && f.pwm_hz==2000 && f.pwm_duty==50);
    tone[2]=101;debug(1,tone,502);assert(f.last[M1_LINK][4]==1 && !r.debug_buzzer);
    tone[2]=50;f.pwm_fail=true;debug(1,tone,503);assert(f.last[M1_LINK][4]==4 && !r.debug_buzzer);f.pwm_fail=false;
    hcho(42,510,false);pm(20,510);pm(20,511);pm(20,512);
    uint8_t sample[7]={0x01,0x2d,0,151,0xfd,0xda,55};
    debug(2,sample,513);tick(513,false);
    assert(r.debug_display && r.debug_state.sample.temperature_centi_c==-550 && f.screen[16]==2);
    cmd(1,0,0,514);assert(f.last[M1_LINK][9]==0 && f.last[M1_LINK][10]==0);
    assert(r.state.hcho_raw==42 && r.measured.pm25==20);
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
        p[2]=(uint8_t)(i%15);bytes(M1_LINK,p,12,i);tick(i,false);
        assert(r.rx[M1_LINK].used<12 && r.rx[M1_PM].used<64);
    }
    m1_runtime_rx_error(&r,(m1_channel)-1);m1_runtime_rx(&r,(m1_channel)-1,0,0);
}
static void set_threshold(uint8_t code,uint16_t low,uint16_t high,uint32_t now)
{
    uint8_t p[12]={0x23,code,(uint8_t)(low>>8),(uint8_t)low,
        (uint8_t)(high>>8),(uint8_t)high,0,0,0,0,0,0x21};
    bytes(M1_LINK,p,sizeof(p),now);
}
static void threshold_tests(void)
{
    init(0);r.sensor_alarm_mask=3;tick(0,false);debug(0,0,0);
    assert(f.last[M1_LINK][6]==0xff && f.last[M1_LINK][7]==0xff);
    debug(9,0,0);
    const uint8_t defaults[9]={0,100,1,44,0,75,0,150,3};
    assert(f.last[M1_LINK][4]==0 && !memcmp(f.last[M1_LINK]+5,defaults,sizeof(defaults)));
    unsigned replies=f.commands[M1_LINK][5];
    set_threshold(5,50,100,1);set_threshold(6,20,40,2);
    assert(f.commands[M1_LINK][5]==replies); /* original writes have no ACK */
    debug(9,0,3);
    const uint8_t changed[9]={0,50,0,100,0,20,0,40,3};
    assert(f.last[M1_LINK][4]==0 && !memcmp(f.last[M1_LINK]+5,changed,sizeof(changed)));
    uint8_t invalid[7]={1};debug(9,invalid,4);assert(f.last[M1_LINK][4]==1);
    set_threshold(5,101,100,5);debug(9,0,6);
    assert(!memcmp(f.last[M1_LINK]+5,changed,sizeof(changed))); /* atomic rejection */
    /* Color updates respect the 3-second snapshot; the two masks stay independent. */
    hcho(51,10,false);r.measured.pm25=20;r.state.sample.pm25_health=1;
    tick(10,false);assert(!r.alarm_latched && !f.buzz && r.display_at==3000);
    assert(r.state.sample.hcho==0 && r.state.sample.pm25==0);
    tick(3000,false);tick(4300,false);
    assert((f.screen[0x12]&0x70)==0x70 && (f.screen[0x13]&0x85)==0x85);
    hcho(100,4301,false);tick(4301,false);assert(!r.alarm_latched);
    hcho(101,4302,false);tick(4302,false);assert(r.alarm_latched && f.buzz);
    tick(4502,false);assert(!f.buzz); /* a single bounded alarm, not permanent on */
    hcho(100,4503,false);tick(4503,false);assert(!r.alarm_latched);
    m1_runtime_beep(&r,M1_BUZZER_OFF,4504);
    r.measured.pm25=40;tick(4504,false);assert(!r.alarm_latched);
    r.measured.pm25=41;tick(4505,false);assert(r.alarm_latched && f.buzz);
    /* Raising a threshold releases the latch without interrupting a playing
     * bounded sequence; a later new crossing can trigger another sequence. */
    set_threshold(6,20,50,4506);tick(4506,false);assert(!r.alarm_latched);
    m1_runtime_beep(&r,M1_BUZZER_OFF,4507);
    set_threshold(6,20,40,4508);tick(4508,false);assert(r.alarm_latched && f.buzz);
    m1_runtime_beep(&r,M1_BUZZER_OFF,4509);
    r.state.sample.hcho_health=r.state.sample.pm25_health=0;
    tick(4509,false);assert(!r.alarm_latched && !f.buzz); /* unhealthy data cannot alarm */
    /* Numeric overlays follow updated settings; raw RAM stays literal. */
    uint8_t sample[7]={0,101,0,41,0,0,50};debug(2,sample,4510);
    set_threshold(5,200,300,4511);set_threshold(6,60,80,4512);tick(4512,false);
    assert((f.screen[0x13]&0xf5)==0xf5);
    uint8_t raw[7]={0x13,1,0xaa};debug(4,raw,4513);
    set_threshold(5,0,0,4514);tick(4514,false);assert(f.screen[0x13]==0xaa);
    cmd(0x12,1,0,4515);debug(9,0,4516);assert(f.last[M1_LINK][4]==2);
    /* RAM-only settings reset to defaults on an MCU reboot. */
    init(0);debug(9,0,0);assert(!memcmp(f.last[M1_LINK]+5,defaults,sizeof(defaults)));
}
static void sensor_alarm_tests(void)
{
    init(0);tick(0,false);debug(0,0,0);assert(f.last[M1_LINK][17]==1);
    hcho(500,1,false);r.measured.pm25=500;r.state.sample.pm25_health=1;
    tick(1,false);assert(!f.buzz && !r.alarm_latched); /* opt in */
    debug(20,0,2);assert(!f.last[M1_LINK][4] && f.last[M1_LINK][5]==1 && !f.last[M1_LINK][6]);
    uint8_t a[7]={1,1};debug(20,a,3);tick(3,false);
    assert(r.sensor_alarm_mask==1 && r.sensor_alarm_playing && f.buzz);
    debug(20,0,4);assert(f.last[M1_LINK][6]==1 && f.last[M1_LINK][7]==1);
    a[1]=0;debug(20,a,5);tick(5,false);assert(!f.buzz && !r.alarm_latched);
    /* PM is independently enabled, even while HCHO is high. */
    a[1]=2;debug(20,a,6);tick(6,false);assert(f.buzz && r.sensor_alarm_playing);
    a[1]=0;debug(20,a,7);tick(7,false);assert(!f.buzz);
    /* Optional threshold alarms cannot cancel manual or clock commands. */
    cmd(0x0e,1,0,8);tick(8,false);assert(f.buzz && !r.sensor_alarm_playing);
    a[1]=3;debug(20,a,9);tick(9,false);assert(f.buzz && r.buzzer_mode==1);
    a[1]=0;debug(20,a,10);tick(10,false);assert(f.buzz && r.buzzer_mode==1);
    cmd(0x0e,0,0,11);tick(11,false);assert(!f.buzz);
    /* Invalid writes are atomic, and read requests must contain no mask. */
    for(unsigned i=0;i<3;++i) {
        uint8_t invalid[7]={1,3};
        if(i==0)invalid[0]=2;
        if(i==1)invalid[1]=8;
        if(i==2)invalid[6]=1;
        debug(20,invalid,12+i);assert(f.last[M1_LINK][4]==1 && !r.sensor_alarm_mask);
    }
    a[0]=0;a[1]=1;debug(20,a,16);assert(f.last[M1_LINK][4]==1);
    /* A mask may change while a finite manual alarm is running. */
    m1_runtime_beep(&r,M1_BUZZER_ALARM,17);a[0]=1;a[1]=3;
    r.state.sample.hcho_health=r.state.sample.pm25_health=0;
    debug(20,a,17);tick(17,false);assert(f.buzz && !r.sensor_alarm_playing);
    a[1]=0;debug(20,a,18);tick(18,false);assert(f.buzz);
    cmd(0x12,1,0,19);debug(20,0,20);assert(f.last[M1_LINK][4]==2);
    init(0);assert(!r.sensor_alarm_mask); /* cold boot is always off */
}
/* Runtime integration: a stored tune owns PWM without blocking acquisition,
 * and ordinary preset/stop/OTA paths reliably release that ownership. */
static bool music_read(void *ctx,uint32_t at,uint8_t *p,size_t n)
{ (void)ctx;(void)at;memset(p,255,n);return true; }
static bool music_erase(void *ctx,uint32_t at){(void)ctx;(void)at;return true;}
static bool music_write(void *ctx,uint32_t at,const uint8_t *p){(void)ctx;(void)at;(void)p;return true;}
static void music_tests(void)
{
    init(0);r.io.flash=(m1_flash){0,music_read,music_erase,music_write};
    const char song[]="Test:d=4,o=5,b=120:c,p,d";
    memcpy(r.music.text,song,sizeof(song));r.music.length=sizeof(song)-1;r.music.present=true;
    uint8_t play[7]={5,0,10};debug(15,play,0);assert(f.last[M1_LINK][4]==0);
    tick(0,false);assert(f.buzz && f.pwm_hz==524 && r.music.playing);
    unsigned requests=f.commands[M1_HCHO][1];tick(500,false);assert(!f.buzz && r.music.playing);
    tick(1000,false);assert(f.buzz && f.pwm_hz==588 && f.commands[M1_HCHO][1]>requests);
    tick(1500,false);assert(!r.music.playing && !f.buzz && f.pwm_hz==2000);
    debug(15,play,2000);tick(2000,false);assert(r.music.playing);
    uint8_t preset[7]={1};debug(14,preset,2001);assert(!r.music.playing && r.debug_buzzer);
    debug(15,play,3000);tick(3000,false);cmd(0x12,1,0,3001);
    assert(r.update.active && !r.music.playing && !r.debug_buzzer && !f.buzz);
    debug(15,play,3002);assert(f.last[M1_LINK][4]==2);
}
static void sensor_frame_tests(void)
{
    init(UINT32_MAX-50);
    uint8_t a[7]={M1_SENSOR_PM};debug(16,a,0);
    assert(!f.last[M1_LINK][4] && !f.last[M1_LINK][6] && f.last[M1_LINK][9]==255);
    debug(0,0,0);assert(f.last[M1_LINK][11]==1 && f.last[M1_LINK][12]==M1_SENSOR_COUNT);
    hcho(513,UINT32_MAX-10,false);
    const m1_sensor_frame *h=&r.sensor_frames[M1_SENSOR_HCHO];
    assert(h->length==9 && h->generation==1 && h->bytes[2]==2 && h->bytes[3]==1);
    uint8_t saved[9];memcpy(saved,h->bytes,9);hcho(514,1,true);
    assert(h->generation==1 && !memcmp(saved,h->bytes,9));
    a[0]=M1_SENSOR_HCHO;debug(16,a,5);
    assert(!f.last[M1_LINK][4] && f.last[M1_LINK][6]==9 && f.last[M1_LINK][12]==16);
    a[1]=1;a[3]=9;a[5]=1;debug(16,a,6);
    assert(!f.last[M1_LINK][4] && !memcmp(f.last[M1_LINK]+10,saved,9));
    a[2]=1;debug(16,a,6);assert(f.last[M1_LINK][4]==1);a[2]=0;
    a[3]=10;debug(16,a,6);assert(f.last[M1_LINK][4]==1);a[3]=9;
    hcho(515,7,false);debug(16,a,8);assert(f.last[M1_LINK][4]==5);
    a[5]=2;a[6]=1;debug(16,a,8);assert(f.last[M1_LINK][4]==1);a[6]=0;
    a[0]=M1_SENSOR_COUNT;debug(16,a,8);assert(f.last[M1_LINK][4]==1);
    init(0);pm(10,1);
    assert(r.sensor_frames[M1_SENSOR_PM].flags & M1_FRAME_WARMING);
    pm(10,2);pm(10,3);pm(1200,4);
    assert(r.sensor_frames[M1_SENSOR_PM].flags & M1_FRAME_FILTERED);
    assert(r.sensor_frames[M1_SENSOR_PM].bytes[12]==4 && r.measured.pm25==10);
    uint16_t gen=r.sensor_frames[M1_SENSOR_PM].generation;
    uint8_t p[32]={0x42,0x4d,0,28};r.pm_armed=true;bytes(M1_PM,p,32,5);
    while(r.pm_armed)m1_runtime_rx(&r,M1_PM,0,5);
    assert(r.sensor_frames[M1_SENSOR_PM].generation==gen);
    p[29]=1;unsigned sum=0;for(unsigned i=0;i<30;++i)sum+=p[i];p[30]=(uint8_t)(sum>>8);p[31]=(uint8_t)sum;
    r.pm_armed=true;bytes(M1_PM,p,32,6);while(r.pm_armed)m1_runtime_rx(&r,M1_PM,0,6);
    assert(r.sensor_frames[M1_SENSOR_PM].flags & M1_FRAME_ERROR);
    init(0);tick(0,false);tick(1,false);tick(21,false);tick(23,false);
    assert(r.sensor_frames[M1_SENSOR_SHT_STATUS].length==3);
    assert(r.sensor_frames[M1_SENSOR_SHT].length==6 && r.sensor_frames[M1_SENSOR_SHT_AUX].length==6);
    assert(r.sensor_frames[M1_SENSOR_SHT].flags & M1_FRAME_CHECKED);
    f.aux_crc_bad=true;tick(1000,false);tick(1001,false);tick(1021,false);tick(1023,false);
    assert(r.sensor_frames[M1_SENSOR_SHT_AUX].length==6);
    assert(!(r.sensor_frames[M1_SENSOR_SHT_AUX].flags & M1_FRAME_CHECKED));
    /* Failed status/measurement reads do not overwrite accepted snapshots. */
    gen=r.sensor_frames[M1_SENSOR_SHT].generation;f.status_crc_bad=true;
    tick(2000,false);tick(2001,false);
    assert(r.sensor_frames[M1_SENSOR_SHT].generation==gen);
    cmd(0x12,1,0,2002);uint8_t meta[7]={0};debug(16,meta,2003);assert(f.last[M1_LINK][4]==2);
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
    clock_tests();button_tests();sensors();sensor_frame_tests();protocol();display();pm_modes();sensor_cadence();pm_cadence();pm_stability();climate_recovery();colon_tests();tone_tests();music_tests();debug_tests();threshold_tests();sensor_alarm_tests();external_display_tests();debug_stream_fuzz();arithmetic_fuzz();
    puts("runtime: PM dual modes/SHT recovery/PA24/debug/protocol/button/clock/display/buzzer/wraparound PASS");
    puts("climate: 50000 bounded random conversions PASS");
}
