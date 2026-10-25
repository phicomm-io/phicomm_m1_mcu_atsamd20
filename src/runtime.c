/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/runtime.h"
#include "m1/display.h"
static bool due(uint32_t now,uint32_t at) { return (int32_t)(now-at)>=0; }
static const uint8_t hcho_probe[9]={0xff,1,0x78,0x41,0,0,0,0,0x46};
static const uint8_t hcho_query[9]={0xff,1,0x86,0,0,0,0,0,0x79};
static const uint8_t pm_probe[7]={0x42,0x4d,0xe2,0,0,1,0x71};
static bool send(m1_runtime *r,m1_channel c,const uint8_t *p,size_t n)
{
    bool ok=r->io.uart(r->io.context,c,p,n);
    if(!ok) r->tx_errors++;
    return ok;
}
static void event(m1_runtime *r,uint8_t code)
{ if(r->update.active && code!=0x14 && code!=0x15 && code!=0x1a) return;
  uint8_t p[20]; m1_event(code,p); (void)send(r,M1_LINK,p,20); }
static void error(m1_runtime *r,uint8_t code)
{ if(r->update.active && code!=7) return;
  uint8_t p[20]; m1_error_event(code,p); (void)send(r,M1_LINK,p,20); }
static void good(m1_runtime *r,unsigned sensor,uint32_t now)
{
    m1_health *h=&r->health[sensor];
    h->last_good=now; h->seen=true; h->fault=false; h->bad=0;
    if(sensor==0) r->state.sample.hcho_health=1;
    if(sensor==1) r->state.sample.pm25_health=1;
    if(sensor==2) r->state.sample.climate_health=1;
}
static void bad(m1_runtime *r,unsigned sensor)
{
    m1_health *h=&r->health[sensor]; h->errors++;
    if(h->bad<3) h->bad++;
    if(h->bad==3 && !h->fault) {
        h->fault=true;
        if(sensor<2) error(r,sensor==0?10:11);
    }
}
static void pm_bad(m1_runtime *r)
{
    bad(r,1);m1_pm_filter_interrupt(&r->pm_filter);
    if(r->health[1].fault) {
        r->state.sample.pm25_health=0;r->pm_warmup=0;
        m1_pm_filter_reset(&r->pm_filter);
    }
}
void m1_runtime_init(m1_runtime *r,const m1_io *io,uint32_t now)
{
    *r=(m1_runtime){0}; r->io=*io; m1_init(&r->state);
    r->co2_threshold[0]=1000;r->co2_threshold[1]=1500;r->co2_interval=5;
    r->state.clock=(m1_clock){2000,1,1,0,0,0};
    if(!io->rtc_read(io->context,&r->state.clock)) r->rtc_errors++;
    if(!m1_clock_valid(&r->state.clock)) r->state.clock=(m1_clock){2000,1,1,0,0,0};
    r->second_at=now+1000; r->display_at=r->climate_at=r->hcho_at=r->pm_at=now;
    r->display_send_at=now+2200;
    r->colon_at=now+500; r->colon_on=true;
    m1_music_load(&r->music,&r->io.flash);
    r->display_retry=now; r->brightness_applied=255; r->dirty=true;
    for(unsigned i=0;i<3;++i) r->health[i].last_good=now;
    r->pm_mode=3; r->pm_auto=r->pm_armed=true; r->climate_reset_at=r->pm_probe_at=now;
    if(io->wifi_enable) io->wifi_enable(io->context,true);
    event(r,0x17);
}
static void command(m1_runtime *r,const uint8_t *p,size_t n,uint32_t now)
{
    uint8_t reply[20];
    /* A committed upgrade owns the reset sequence; do not start new work. */
    if(r->reset_pending) return;
    if(p[1]==0x70) {
        if(r->update.active) { m1_event(0x70,reply); reply[2]=p[2]; reply[3]=p[3]; reply[4]=2; }
        else (void)m1_debug_command(r,p,reply,now);
        (void)send(r,M1_LINK,reply,20); return;
    }
    if(p[1]==0x12) {
        if(m1_update_begin(&r->update,p[2],p[3],now)) {
            r->music.uploading=false;
            uint8_t leave[12]={0x23,0x70,5,0,0,0,0,0,0,0,0,0x21};
            (void)m1_debug_command(r,leave,reply,now); event(r,0x14);
        }
        else error(r,7);
        return;
    }
    if(p[1]==0x13) {
        m1_update_block(&r->update,&r->io.flash,p,n,now,reply);
        (void)send(r,M1_LINK,reply,20);
        if(r->update.complete && reply[4]==1) {
            m1_event(0x15,reply); reply[2]=1; (void)send(r,M1_LINK,reply,20);
        }
        return;
    }
    if(p[1]==0x16) {
        if(p[2]==1) { m1_update_abort(&r->update); event(r,0x1a); }
        else if(m1_update_commit(&r->update,&r->io.flash)) {
            r->reset_pending=true; r->reset_phase=0; r->reset_at=now+4000; r->state.display_on=false; r->dirty=true;
        } else error(r,7);
        return;
    }
    m1_result result=m1_command(&r->state,p,reply);
    if(result==M1_REPLY) (void)send(r,M1_LINK,reply,20);
    if(result==M1_BAD_FRAME) { r->rx_errors++; error(r,7); }
    if(result==M1_APPLIED) {
        r->dirty=true;
        if((p[1]==5 || p[1]==6) && r->debug_display) {
            /* Value overlays use current configured colors. Raw segment
             * overrides remain literal bytes until the overlay expires. */
            for(unsigned i=0;i<2;++i) {
                r->debug_state.hcho_threshold[i]=r->state.hcho_threshold[i];
                r->debug_state.pm25_threshold[i]=r->state.pm25_threshold[i];
            }
        }
        if(p[1]==3) {
            r->second_at=now+1000;
            r->colon_at=now+500; r->colon_on=true;
            if(!r->io.rtc_write(r->io.context,&r->state.clock)) r->rtc_errors++;
        }
        if(p[1]==0x0e) {
            r->buzzer_mode=r->state.buzzer_on?1:0;r->buzzer_at=now;r->sensor_alarm_playing=false;
        }
    }
}
/* Main-loop owned cache. Readers use generation checks to avoid joining
 * chunks from different acquisitions. Capture never changes sampling cadence. */
static void capture(m1_runtime *r,unsigned sensor,const uint8_t *p,unsigned n,uint8_t flags,uint32_t now)
{
    m1_sensor_frame *f=&r->sensor_frames[sensor];
    for(unsigned i=0;i<n;++i) f->bytes[i]=p[i];
    f->length=(uint8_t)n; f->flags=(uint8_t)(flags|M1_FRAME_PRESENT);
    f->received_at=now; f->generation++;
}
static void sample_hcho(m1_runtime *r,const uint8_t *p,uint32_t now)
{
    unsigned sum=0; for(unsigned i=1;i<9;++i) sum+=p[i];
    if((sum&255)!=0) { bad(r,0); return; }
    if(p[1]==0x78) { capture(r,M1_SENSOR_HCHO_PROBE,p,9,M1_FRAME_CHECKED,now); r->hcho_probe=3; r->hcho_at=now; return; }
    uint16_t value;
    if(!m1_hcho_raw(p,&value)) { bad(r,0); return; }
    capture(r,M1_SENSOR_HCHO,p,9,M1_FRAME_CHECKED,now);
    r->hcho_probe=3; r->state.hcho_raw=value;
    good(r,0,now);
}
void m1_runtime_pm_mode(m1_runtime *r,uint8_t mode,uint32_t now)
{
    if(mode!=0 && mode!=2 && mode!=3) return;
    r->pm_auto=mode==0; r->pm_probing=false; r->pm_probe_used=0;
    r->pm_mode=mode?mode:3; r->pm_warmup=0;
    m1_pm_filter_reset(&r->pm_filter);
    r->measured.pm25=0; r->state.sample.pm25_health=0;
    r->rx[M1_PM].used=0; r->pm_at=r->pm_probe_at=now; r->pm_armed=true;
}
static bool pm_valid(const uint8_t *p)
{
    m1_pm_frame decoded;
    return m1_pm_decode(p,&decoded);
}
static void sample_pm(m1_runtime *r,const uint8_t *p,uint32_t now,bool warming)
{
    /* Both sensor modes use one filtered value per acquisition. The cached
     * frame remains byte-for-byte raw, including an error or rejected spike. */
    uint8_t flags=M1_FRAME_CHECKED;
    if(p[29]) flags|=M1_FRAME_ERROR;
    if(warming) flags|=M1_FRAME_WARMING;
    capture(r,M1_SENSOR_PM,p,32,flags,now);
    m1_pm_frame decoded;
    (void)m1_pm_decode(p,&decoded);
    uint16_t value=decoded.atmospheric[1];
    if(p[29]) {
        pm_bad(r);return;
    }
    if(r->health[1].seen && now-r->health[1].last_good>6000) {
        m1_pm_filter_reset(&r->pm_filter);warming=true;r->pm_warmup=1;
        r->sensor_frames[M1_SENSOR_PM].flags|=M1_FRAME_WARMING;
    }
    good(r,1,now);
    if(warming) {
        m1_pm_filter_reset(&r->pm_filter);r->measured.pm25=0;
        r->state.sample.pm25_health=0;return;
    }
    bool rejected;
    r->measured.pm25=m1_pm_filter_add(&r->pm_filter,value,&rejected);
    if(rejected) r->sensor_frames[M1_SENSOR_PM].flags|=M1_FRAME_FILTERED;
}
static void pm_rx(m1_runtime *r,uint8_t byte,uint32_t now)
{
    /* Auto-detection is an additional recovery policy. Keep its short ACK
     * recognizer separate from stock-sized data windows and frame gaps. */
    if(r->pm_probing) {
        if(r->pm_probe_used<8) r->pm_probe_rx[r->pm_probe_used++]=byte;
        else {
            for(unsigned i=0;i<7;++i) r->pm_probe_rx[i]=r->pm_probe_rx[i+1];
            r->pm_probe_rx[7]=byte;
        }
        const uint8_t *p=r->pm_probe_rx;
        if(r->pm_probe_used==8 && p[0]==0x42 && p[1]==0x4d &&
           (p[2]!=0 || p[3]!=28) && p[6]==1 && p[7]==0x74) {
            capture(r,M1_SENSOR_PM_PROBE,p,8,0,now);
            if(r->pm_mode!=2) {
                r->pm_mode=2;r->pm_warmup=0;m1_pm_filter_reset(&r->pm_filter);
                r->measured.pm25=0;r->state.sample.pm25_health=0;
            }
            r->pm_probing=false; r->rx[M1_PM].used=0; r->pm_armed=true; return;
        }
    }
    if(!r->pm_armed) return;
    m1_rx *s=&r->rx[M1_PM];
    s->bytes[s->used++]=byte; s->last=now;
    unsigned count=r->pm_mode==2?32:64;
    if(s->used<count) return;
    r->pm_armed=false; s->used=0;
    bool warming=r->pm_warmup<(r->pm_mode==2?4:2);
    /* Stock mode 3 warms up by complete 64-byte callbacks, even if corrupt.
     * Mode 2 increments only after a checksum-valid 32-byte callback. */
    if(r->pm_mode==3 && warming) { r->pm_warmup++; r->measured.pm25=0; }
    for(unsigned offset=0;offset<(r->pm_mode==2?1:32);++offset) {
        if(!pm_valid(s->bytes+offset)) continue;
        if(r->pm_mode==2 && warming) r->pm_warmup++;
        sample_pm(r,s->bytes+offset,now,warming); return;
    }
    pm_bad(r);
}
void m1_runtime_rx_error(m1_runtime *r,m1_channel channel)
{
    if((unsigned)channel>M1_PM) return;
    r->rx[channel].used=0; r->rx_errors++;
    if(channel==M1_HCHO) bad(r,0);
    if(channel==M1_PM) pm_bad(r);
}
void m1_runtime_rx(m1_runtime *r,m1_channel channel,uint8_t byte,uint32_t now)
{
    if((unsigned)channel>M1_PM || r->reset_phase) return;
    if(channel==M1_PM) { pm_rx(r,byte,now); return; }
    m1_rx *s=&r->rx[channel];
    if(s->used && now-s->last>100) { s->used=0; r->rx_errors++; }
    s->last=now;
    uint8_t head=channel==M1_LINK?0x23:0xff;
    if(!s->used && byte!=head) return;
    s->bytes[s->used++]=byte;
    size_t expected=channel==M1_LINK?12:9;
    if(channel==M1_LINK && s->used>=4 && s->bytes[1]==0x13) {
        uint16_t block=(uint16_t)((uint16_t)s->bytes[2]<<8|s->bytes[3]);
        expected=m1_update_frame_size(&r->update,block);
        if(!expected) { s->used=0; r->rx_errors++; return; }
    }
    if(s->used<expected) return;
    bool valid=channel!=M1_LINK || s->bytes[1]==0x13 || s->bytes[11]==0x21;
    if(valid) {
        if(channel==M1_LINK) command(r,s->bytes,expected,now);
        else sample_hcho(r,s->bytes,now);
        s->used=0;
    } else {
        unsigned skip=1;
        while(skip<s->used && s->bytes[skip]!=head) skip++;
        s->used=(uint16_t)(s->used-skip);
        for(unsigned i=0;i<s->used;++i) s->bytes[i]=s->bytes[i+skip];
        r->rx_errors++;
    }
}
static bool climate_cmd(m1_runtime *r,uint16_t command)
{
    uint8_t tx[2]={(uint8_t)(command>>8),(uint8_t)command};
    return r->io.i2c(r->io.context,2,0x44,tx,2,0,0);
}
static void climate_failed(m1_runtime *r,uint32_t now)
{
    bad(r,2); r->climate_phase=0; r->climate_status_valid=false;
    if(r->health[2].bad>=3) r->climate_reset_due=true;
    r->climate_at=now+1000;
}
static void climate(m1_runtime *r,uint32_t now)
{
    if(!due(now,r->climate_at)) return;
    uint8_t data[6];
    if(!r->climate_phase) {
        if(r->climate_reset_due && due(now,r->climate_reset_at)) {
            r->climate_reset_at=now+10000; /* Bound reset storms, even on NACK. */
            if(!climate_cmd(r,0x30a2)) { climate_failed(r,now); return; }
            r->climate_resets++; r->climate_reset_due=false;
            r->climate_status_valid=false; r->climate_phase=5; r->climate_at=now+2; return;
        }
        if(climate_cmd(r,0xf32d)) { r->climate_phase=3; r->climate_at=now+1; return; }
    } else if(r->climate_phase==5) {
        /* Soft reset needs up to 1.5 ms (SHT3x-DIS v7). */
        r->climate_phase=0; r->climate_at=now; return;
    } else if(r->climate_phase==3) {
        if(r->io.i2c(r->io.context,2,0x44,0,0,data,3) && m1_sht_crc(data,2)==data[2]) {
            capture(r,M1_SENSOR_SHT_STATUS,data,3,M1_FRAME_CHECKED,now);
            r->climate_status=(uint16_t)((uint16_t)data[0]<<8|data[1]);
            r->climate_status_valid=true;
            /* Alert/reset-detected flags are informational. A bad last
             * command/write contributes to failure, never a reset loop alone. */
            if(!(r->climate_status&3) && climate_cmd(r,0x2400)) {
                r->climate_phase=1; r->climate_at=now+20; return;
            }
        }
    } else if(r->climate_phase==1) {
        if(r->io.i2c(r->io.context,2,0x44,0,0,data,6) && m1_sht_raw(data,&r->raw_t,&r->raw_h)) {
            capture(r,M1_SENSOR_SHT,data,6,M1_FRAME_CHECKED,now);
            if(climate_cmd(r,0x3682)) { r->climate_phase=2; r->climate_at=now+2; return; }
        }
    } else if(r->climate_phase==2 && r->io.i2c(r->io.context,2,0x44,0,0,data,6)) {
        /* Preserve this original extra response even if its CRC is bad.
         * Its physical meaning is unknown; do not change stock compensation. */
        bool checked=m1_sht_crc(data,2)==data[2] && m1_sht_crc(data+3,2)==data[5];
        capture(r,M1_SENSOR_SHT_AUX,data,6,checked?M1_FRAME_CHECKED:0,now);
        int32_t t; uint32_t rh;
        uint32_t aux=(uint32_t)data[0]<<24|(uint32_t)data[1]<<16|(uint32_t)data[3]<<8|data[4];
        m1_climate next=r->climate;
        if(m1_climate_convert(&next,r->raw_t,r->raw_h,aux,
                             (r->debug_display ? (r->debug_state.display_on && r->debug_state.brightness) :
                              (r->state.display_on && r->state.brightness)),&t,&rh)) {
            r->climate=next;
            r->measured.temperature_centi_c=(int16_t)(t/10);
            r->measured.humidity_centi_pct=(uint16_t)(rh>100000?10000:rh/10);
            good(r,2,now); r->climate_reset_due=false;
            r->climate_phase=0; r->climate_at=now+977; return;
        }
    }
    climate_failed(r,now);
}
static void render(m1_runtime *r,uint32_t now)
{
    if(!due(now,r->display_at)) return;
    /* Stock sem_task wakes led_task at 0, 3000, 6000 ... ticks. Do not replay
     * old samples after a stall or couple this phase to screen I/O/control. */
    r->display_at+=((now-r->display_at)/3000+1)*3000;
    if(r->state.display_test) return; /* Stock all-segment branch skips sampling. */
    r->state.sample.pm25=r->measured.pm25;
    r->state.sample.temperature_centi_c=r->measured.temperature_centi_c;
    r->state.sample.humidity_centi_pct=r->measured.humidity_centi_pct;
    if(r->state.hcho_raw>9999) { /* Stock retains the previous reply value. */
    } else if(r->state.hcho_raw<11) {
        r->state.sample.hcho=r->low_hcho_seen?10:0; r->low_hcho_seen=true;
    } else { r->state.sample.hcho=r->state.hcho_raw; r->low_hcho_seen=true; }
    m1_display_render(&r->state,r->display_ram);
}
void m1_runtime_display_ram(const m1_runtime *r,uint8_t ram[M1_DISPLAY_BYTES])
{
    const m1_state *shown=r->debug_display?&r->debug_state:&r->state;
    if(r->debug_display && r->debug_raw) {
        for(unsigned i=0;i<M1_DISPLAY_BYTES;++i) ram[i]=r->debug_ram[i];
        return; /* Raw diagnostics must remain literal. */
    }
    if(r->debug_display) m1_display_render(shown,ram);
    else for(unsigned i=0;i<M1_DISPLAY_BYTES;++i)
        ram[i]=shown->display_test?0xff:r->display_ram[i];
    if(shown->display_test) return;
    /* Both colon dots share bit 3 with neither digit's seven segments. */
    ram[0]=(uint8_t)((ram[0]&~8u)|(r->colon_on?8u:0));
    ram[3]=(uint8_t)((ram[3]&~8u)|(r->colon_on?8u:0));
    if(r->debug_display) return;
    if(r->external_dual) {
        if(r->external_dual_source) for(unsigned i=0;i<2;i++) {
            const m1_display_lane *lane=&r->external_lanes[i];
            if(lane->valid) m1_display_external(ram,(uint8_t)i,lane->number,
                                               lane->format,lane->color);
        }
    } else {
        if(r->co2_show) m1_display_co2(ram,r->co2_slot,r->co2_ppm,r->co2_threshold);
        if(r->external_source && r->external_valid)
            m1_display_external(ram,r->external_slot,r->external_number,
                                r->external_format,r->external_color);
    }
}
static void display(m1_runtime *r,uint32_t now)
{
    m1_co2_poll(r,now);
    render(r,now);
    if(due(now,r->colon_at)) {
        uint32_t phases=(now-r->colon_at)/500+1;
        r->colon_at+=phases*500;
        if(phases&1) {
            r->colon_on=!r->colon_on;
            const m1_state *shown=r->debug_display?&r->debug_state:&r->state;
            if(shown->display_on && shown->brightness && !shown->display_test &&
               !(r->debug_display && r->debug_raw)) r->dirty=true;
        }
    }
    if(!due(now,r->display_retry) || (!r->dirty && !due(now,r->display_send_at))) return;
    const uint8_t init[]={0x80,0xa0,0x9a,0x81,0x83,0xbf,0x88};
    if(!r->display_ready) {
        for(unsigned i=0;i<sizeof(init);++i)
            if(!r->io.i2c(r->io.context,0,0x72,init+i,1,0,0)) goto failed;
        r->display_ready=true; r->brightness_applied=255;
    }
    const m1_state *shown=r->debug_display?&r->debug_state:&r->state;
    bool on=shown->display_on && shown->brightness;
    uint8_t control=on?0x83:0x82;
    if(r->brightness_applied!=shown->brightness || r->display_applied!=on) {
        if(!r->io.i2c(r->io.context,0,0x72,&control,1,0,0)) goto failed;
        if(on) {
            control=m1_display_brightness(shown->brightness);
            if(!r->io.i2c(r->io.context,0,0x72,&control,1,0,0)) goto failed;
        }
        r->display_applied=on; r->brightness_applied=shown->brightness;
    }
    uint8_t packet[30]={0};
    m1_runtime_display_ram(r,packet+1);
    if(!r->io.i2c(r->io.context,0,0x72,packet,30,0,0)) goto failed;
    r->dirty=false;
    /* Stock 100-tick callback tests counter >20 before resetting to zero,
     * then increments it: first transfer at 2200, subsequent ones every 2100.
     * Immediate control/debug writes must not move this periodic deadline. */
    if(due(now,r->display_send_at)) r->display_send_at+=((now-r->display_send_at)/2100+1)*2100;
    return;
failed:
    r->display_errors++; r->display_ready=false; r->dirty=true; r->display_retry=now+1000;
}
static void button_event(m1_runtime *r,uint8_t kind,uint32_t held)
{
    uint8_t reply[20];m1_event(0x71,reply);
    reply[2]=1;reply[3]=kind;reply[4]=++r->button_sequence;
    if(held>65535)held=65535;
    reply[5]=(uint8_t)(held>>8);reply[6]=(uint8_t)held;
    (void)send(r,M1_LINK,reply,sizeof(reply));
}
static void button_single(m1_runtime *r)
{
    uint8_t reply[20];r->button_click_pending=false;
    button_event(r,1,0);r->state.display_test=false;
    m1_button_short(&r->state,reply);(void)send(r,M1_LINK,reply,sizeof(reply));r->dirty=true;
}
static void button(m1_runtime *r,uint32_t now,bool pressed)
{
    if(r->update.active || r->reset_pending){
        r->button_click_pending=false;r->button_down=r->button_raw=pressed;
        r->button_at=r->edge_at=now;r->button_3s=r->button_10s=false;return;
    }
    if(r->button_click_pending && (uint32_t)(now-r->button_click_at)>350)button_single(r);
    if(pressed!=r->button_raw) { r->button_raw=pressed; r->edge_at=now; }
    if(now-r->edge_at<20) return;
    if(pressed!=r->button_down) {
        r->button_down=pressed;
        if(pressed) { r->button_at=r->edge_at; r->button_3s=r->button_10s=false; }
        else {
            uint32_t held=r->edge_at-r->button_at;
            if(held>=60 && held<3000) {
                if(r->button_click_pending){r->button_click_pending=false;button_event(r,2,held);}
                else {r->button_click_pending=true;r->button_click_at=now;}
            } else if(held>=9000) event(r,0x18);
        }
    }
    if(r->button_down) {
        uint32_t held=now-r->button_at;
        if(held>=3010 && !r->button_3s) { r->button_3s=true;button_event(r,3,held);event(r,4); }
        if(held>=10000 && !r->button_10s) {
            r->button_10s=true; r->state.display_test=true; r->dirty=true;button_event(r,4,held);event(r,0x10);
        }
    }
}
void m1_runtime_beep(m1_runtime *r,m1_buzzer_mode mode,uint32_t now)
{
    if(mode>M1_BUZZER_ALARM) return;
    r->buzzer_mode=(uint8_t)mode; r->buzzer_at=now;
    r->sensor_alarm_playing=false;
    r->state.buzzer_on=mode==M1_BUZZER_CONTINUOUS;
}
static void buzzer(m1_runtime *r,uint32_t now)
{
    if(r->music.playing || r->music_applied) {
        uint16_t hz=0;unsigned action=m1_music_tick(&r->music,now,&hz);
        if(action) {
            r->music_applied=true;
            if(action==2) {
                if(hz && !r->io.buzzer_pwm(r->io.context,hz,50))m1_music_stop(&r->music);
                else { r->io.buzzer(r->io.context,hz!=0);return; }
            } else return;
        }
        r->music_applied=false;
        if(r->io.buzzer_pwm)(void)r->io.buzzer_pwm(r->io.context,2000,50);
        r->io.buzzer(r->io.context,false);r->buzzer_applied=false;
    }
    if(r->debug_buzzer) return;
    bool alarm=((r->sensor_alarm_mask&1) && r->state.sample.hcho_health &&
                m1_threshold_group(r->state.hcho_raw,r->state.hcho_threshold)==M1_GROUP_RED)||
        ((r->sensor_alarm_mask&2) && r->state.sample.pm25_health &&
         m1_threshold_group(r->measured.pm25,r->state.pm25_threshold)==M1_GROUP_RED)||
        ((r->sensor_alarm_mask&4) && r->co2_mode && r->co2_valid &&
         m1_threshold_group(r->co2_ppm,r->co2_threshold)==M1_GROUP_RED);
    if(alarm && !r->alarm_latched && r->buzzer_mode!=1) {
        r->buzzer_mode=3;r->buzzer_at=now;r->sensor_alarm_playing=true;
    }
    r->alarm_latched=alarm;
    uint32_t elapsed=now-r->buzzer_at;
    bool on=r->buzzer_mode==1;
    if(r->buzzer_mode==2) { on=elapsed<200; if(!on) r->buzzer_mode=0; }
    if(r->buzzer_mode==3) {
        if(elapsed>=6400) { r->buzzer_mode=0;r->sensor_alarm_playing=false; }
        else { uint32_t t=elapsed%800; on=t<200 || t>=600; }
    }
    if(on!=r->buzzer_applied) { r->io.buzzer(r->io.context,on); r->buzzer_applied=on; }
}
void m1_runtime_poll(m1_runtime *r,uint32_t now,bool pressed)
{
    if(r->reset_pending && due(now,r->reset_at)) {
        if(!r->reset_phase) {
            if(r->io.wifi_enable) r->io.wifi_enable(r->io.context,false);
            r->reset_phase=1; r->reset_at=now+1000;
        } else { r->reset_pending=false; r->io.reset(r->io.context); }
    }
    if(r->reset_phase) return;
    m1_debug_poll(r,now);
    if(r->music.uploading && (uint32_t)(now-r->music.upload_at)>=30000)r->music.uploading=false;
    if(due(now,r->second_at)) {
        /* Bound catch-up after long stalls; deadlines use modulo-32 arithmetic. */
        unsigned n=(now-r->second_at)/1000+1;
        if(n>60) { n=60; r->second_at=now+1000; }
        else r->second_at+=n*1000;
        for(unsigned i=0;i<n;++i) m1_clock_second(&r->state.clock);
        m1_clock c;
        if(r->io.rtc_read(r->io.context,&c) && m1_clock_valid(&c)) r->state.clock=c;
        else r->rtc_errors++;
        uint8_t cmd[12]={0x23,0x0d,0,0,0,0,0,0,0,0,0,0x21},reply[20];
        (void)m1_command(&r->state,cmd,reply); reply[1]=0x0c;
        if(!r->update.active) (void)send(r,M1_LINK,reply,20);
    }
    if(due(now,r->hcho_at)) {
        bool probe=r->hcho_probe<3;
        (void)send(r,M1_HCHO,probe?hcho_probe:hcho_query,9);
        if(probe) r->hcho_probe++;
        r->hcho_at=now+(probe?500:1000);
    }
    if(r->pm_probing && due(now,r->pm_probe_until)) r->pm_probing=false;
    if(due(now,r->pm_at)) {
        /* Stock pm25_task is woken every third 1000-tick sem_task pass.
         * A busy receive job is left intact, not restarted at a partial frame. */
        r->pm_armed=true;
        if(r->pm_mode==2) (void)send(r,M1_PM,pm_probe,7);
        r->pm_at+=((now-r->pm_at)/3000+1)*3000;
        if(r->health[1].fault) { r->pm_warmup=0;m1_pm_filter_reset(&r->pm_filter); }
        if(r->health[0].fault) r->hcho_probe=0;
    }
    if(r->pm_auto && r->pm_mode==3 && due(now,r->pm_probe_at)) {
        if(send(r,M1_PM,pm_probe,7)) {
            r->pm_probing=true; r->pm_probe_used=0; r->pm_probe_until=now+1000;
        }
        r->pm_probe_at=now+10000;
    }
    for(unsigned i=0;i<3;++i) {
        m1_health *h=&r->health[i];
        if(now-h->last_good>6000 && !h->fault) {
            h->fault=true; h->timeouts++;
            if(i==0) { r->state.sample.hcho_health=0; r->hcho_probe=0; error(r,8); }
            if(i==1) {
                r->state.sample.pm25_health=0;r->pm_warmup=0;
                m1_pm_filter_reset(&r->pm_filter);r->rx[M1_PM].used=0;error(r,9);
            }
            if(i==2) { r->state.sample.climate_health=0; r->climate_reset_due=true; }
        }
        /* Invalid frames must clear health as well as stale samples. */
        if(h->fault) {
            if(i==0) r->state.sample.hcho_health=0;
            if(i==1) r->state.sample.pm25_health=0;
            if(i==2) r->state.sample.climate_health=0;
        }
    }
    climate(r,now); button(r,now,pressed); display(r,now); buzzer(r,now);
    if(r->update.active && now-r->update.last_activity>30000) m1_update_abort(&r->update);
}
