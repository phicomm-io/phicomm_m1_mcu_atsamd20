/* SPDX-License-Identifier: MIT */
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
void m1_runtime_init(m1_runtime *r,const m1_io *io,uint32_t now)
{
    *r=(m1_runtime){0}; r->io=*io; m1_init(&r->state);
    r->state.clock=(m1_clock){2000,1,1,0,0,0};
    if(!io->rtc_read(io->context,&r->state.clock)) r->rtc_errors++;
    if(!m1_clock_valid(&r->state.clock)) r->state.clock=(m1_clock){2000,1,1,0,0,0};
    r->second_at=now+1000; r->display_at=r->climate_at=r->hcho_at=r->pm_at=now;
    r->display_retry=now; r->brightness_applied=255; r->dirty=true;
    for(unsigned i=0;i<3;++i) r->health[i].last_good=now;
    r->pm_mode=3; r->pm_auto=true; r->climate_reset_at=now;
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
        if(p[1]==3) {
            r->second_at=now+1000;
            if(!r->io.rtc_write(r->io.context,&r->state.clock)) r->rtc_errors++;
        }
        if(p[1]==0x0e) { r->buzzer_mode=r->state.buzzer_on?1:0; r->buzzer_at=now; }
    }
}
static void sample_hcho(m1_runtime *r,const uint8_t *p,uint32_t now)
{
    unsigned sum=0; for(unsigned i=1;i<9;++i) sum+=p[i];
    if((sum&255)!=0) { bad(r,0); return; }
    if(p[1]==0x78) { r->hcho_probe=3; r->hcho_at=now; return; }
    uint16_t value;
    if(!m1_hcho_raw(p,&value)) { bad(r,0); return; }
    r->hcho_probe=3; r->state.hcho_raw=value;
    good(r,0,now); r->dirty=true;
}
void m1_runtime_pm_mode(m1_runtime *r,uint8_t mode,uint32_t now)
{
    if(mode!=0 && mode!=2 && mode!=3) return;
    r->pm_auto=mode==0; r->pm_probing=false;
    r->pm_mode=mode?mode:3; r->pm_warmup=0; r->pm_jump=false;
    r->pm_previous=0; r->rx[M1_PM].used=0; r->pm_at=now;
}
static void sample_pm(m1_runtime *r,const uint8_t *p,uint32_t now)
{
    /* Both stock branches carry the same 32-byte data frame. A valid sensor
     * error frame publishes zero in stock, but must not claim healthy data. */
    unsigned sum=0; for(unsigned i=0;i<30;++i) sum+=p[i];
    if(p[0]!=0x42 || p[1]!=0x4d || p[2]!=0 || p[3]!=28 ||
       sum!=((unsigned)p[30]<<8|p[31])) { bad(r,1); return; }
    uint16_t value=(uint16_t)((uint16_t)p[12]<<8|p[13]);
    if(p[29]) { bad(r,1); value=0; } else good(r,1,now);
    unsigned warmup=r->pm_mode==2?4:2;
    if(r->pm_warmup<warmup) { r->pm_warmup++; value=0; }
    else if(r->pm_mode==3 && r->pm_previous<=149 && value>=1001 && !r->pm_jump) {
        r->pm_jump=true; return;
    } else r->pm_jump=false;
    r->pm_previous=value; r->state.sample.pm25=value; r->dirty=true;
}
void m1_runtime_rx_error(m1_runtime *r,m1_channel channel)
{
    if((unsigned)channel>M1_PM) return;
    r->rx[channel].used=0; r->rx_errors++;
    if(channel!=M1_LINK) bad(r,channel==M1_HCHO?0:1);
}
void m1_runtime_rx(m1_runtime *r,m1_channel channel,uint8_t byte,uint32_t now)
{
    if((unsigned)channel>M1_PM || r->reset_phase) return;
    m1_rx *s=&r->rx[channel];
    if(s->used && now-s->last>100) { s->used=0; r->rx_errors++; }
    s->last=now;
    uint8_t head=channel==M1_LINK?0x23:channel==M1_HCHO?0xff:0x42;
    if(!s->used && byte!=head) return;
    s->bytes[s->used++]=byte;
    size_t expected=channel==M1_LINK?12:channel==M1_HCHO?9:32;
    if(channel==M1_PM && s->used==2 && byte!=0x4d) {
        s->used=byte==head?1:0; return;
    }
    if(channel==M1_PM && r->pm_probing && s->used>=4 &&
       (s->bytes[2]!=0 || s->bytes[3]!=28)) {
        if(s->used<8) return;
        if(s->bytes[0]==0x42 && s->bytes[1]==0x4d && s->bytes[6]==1 && s->bytes[7]==0x74) {
            if(r->pm_mode!=2) { r->pm_mode=2; r->pm_warmup=0; r->pm_jump=false; r->pm_previous=0; }
            r->pm_probing=false; s->used=0; return;
        }
        /* Invalid short response: retain a possible embedded next header. */
        unsigned skip=1; while(skip<s->used && s->bytes[skip]!=0x42) skip++;
        s->used=(uint16_t)(s->used-skip);
        for(unsigned i=0;i<s->used;++i) s->bytes[i]=s->bytes[i+skip];
        r->rx_errors++; return;
    }
    if(channel==M1_LINK && s->used>=4 && s->bytes[1]==0x13) {
        uint16_t block=(uint16_t)((uint16_t)s->bytes[2]<<8|s->bytes[3]);
        expected=m1_update_frame_size(&r->update,block);
        if(!expected) { s->used=0; r->rx_errors++; return; }
    }
    if(s->used<expected) return;
    bool valid=channel!=M1_LINK || s->bytes[1]==0x13 || s->bytes[11]==0x21;
    if(valid) {
        if(channel==M1_LINK) command(r,s->bytes,expected,now);
        else if(channel==M1_HCHO) sample_hcho(r,s->bytes,now);
        else {
            unsigned sum=0; for(unsigned i=0;i<30;++i) sum+=s->bytes[i];
            if(s->bytes[2]!=0 || s->bytes[3]!=28 || sum!=((unsigned)s->bytes[30]<<8|s->bytes[31])) {
                bad(r,1); unsigned skip=1;
                while(skip<s->used && s->bytes[skip]!=0x42) skip++;
                s->used=(uint16_t)(s->used-skip);
                for(unsigned i=0;i<s->used;++i) s->bytes[i]=s->bytes[i+skip];
                return;
            }
            sample_pm(r,s->bytes,now);
        }
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
            if(climate_cmd(r,0x3682)) { r->climate_phase=2; r->climate_at=now+2; return; }
        }
    } else if(r->climate_phase==2 && r->io.i2c(r->io.context,2,0x44,0,0,data,6)) {
        int32_t t; uint32_t rh;
        uint32_t aux=(uint32_t)data[0]<<24|(uint32_t)data[1]<<16|(uint32_t)data[3]<<8|data[4];
        m1_climate next=r->climate;
        if(m1_climate_convert(&next,r->raw_t,r->raw_h,aux,
                             (r->debug_display ? (r->debug_state.display_on && r->debug_state.brightness) :
                              (r->state.display_on && r->state.brightness)),&t,&rh)) {
            r->climate=next;
            r->state.sample.temperature_centi_c=(int16_t)(t/10);
            r->state.sample.humidity_centi_pct=(uint16_t)(rh>100000?10000:rh/10);
            good(r,2,now); r->climate_reset_due=false; r->dirty=true;
            r->climate_phase=0; r->climate_at=now+977; return;
        }
    }
    climate_failed(r,now);
}
static void display(m1_runtime *r,uint32_t now)
{
    if(!due(now,r->display_retry) || (!r->dirty && !due(now,r->display_at))) return;
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
    /* Stock low-value normalization is applied on display refresh, not RX. */
    if(r->state.hcho_raw>9999) { /* Stock leaves the previous transmitted value. */
    } else if(r->state.hcho_raw<11) {
        r->state.sample.hcho=r->low_hcho_seen?10:0; r->low_hcho_seen=true;
    } else { r->state.sample.hcho=r->state.hcho_raw; r->low_hcho_seen=true; }
    if(r->debug_display && r->debug_raw) {
        for(unsigned i=0;i<M1_DISPLAY_BYTES;++i) packet[i+1]=r->debug_ram[i];
    } else m1_display_render(shown,packet+1);
    if(!r->io.i2c(r->io.context,0,0x72,packet,30,0,0)) goto failed;
    r->dirty=false; r->display_at=now+1000; return;
failed:
    r->display_errors++; r->display_ready=false; r->dirty=true; r->display_retry=now+1000;
}
static void button(m1_runtime *r,uint32_t now,bool pressed)
{
    if(pressed!=r->button_raw) { r->button_raw=pressed; r->edge_at=now; }
    if(now-r->edge_at<20) return;
    if(pressed!=r->button_down) {
        r->button_down=pressed;
        if(pressed) { r->button_at=r->edge_at; r->button_3s=r->button_10s=false; }
        else {
            uint32_t held=r->edge_at-r->button_at;
            uint8_t reply[20];
            if(held>=60 && held<3000) {
                r->state.display_test=false; m1_button_short(&r->state,reply);
                (void)send(r,M1_LINK,reply,20); r->dirty=true;
            } else if(held>=9000) event(r,0x18);
        }
    }
    if(r->button_down) {
        uint32_t held=now-r->button_at;
        if(held>=3010 && !r->button_3s) { r->button_3s=true; event(r,4); }
        if(held>=10000 && !r->button_10s) {
            r->button_10s=true; r->state.display_test=true; r->dirty=true; event(r,0x10);
        }
    }
}
void m1_runtime_beep(m1_runtime *r,m1_buzzer_mode mode,uint32_t now)
{
    if(mode>M1_BUZZER_ALARM) return;
    r->buzzer_mode=(uint8_t)mode; r->buzzer_at=now;
    r->state.buzzer_on=mode==M1_BUZZER_CONTINUOUS;
}
static void buzzer(m1_runtime *r,uint32_t now)
{
    if(r->debug_buzzer) return;
    bool alarm=(r->state.sample.hcho_health && r->state.hcho_raw>300)||
        (r->state.sample.pm25_health && r->state.sample.pm25>150);
    if(alarm && !r->alarm_latched && r->buzzer_mode!=1) { r->buzzer_mode=3; r->buzzer_at=now; }
    r->alarm_latched=alarm;
    uint32_t elapsed=now-r->buzzer_at;
    bool on=r->buzzer_mode==1;
    if(r->buzzer_mode==2) { on=elapsed<200; if(!on) r->buzzer_mode=0; }
    if(r->buzzer_mode==3) {
        if(elapsed>=6400) r->buzzer_mode=0;
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
        r->dirty=true;
    }
    if(due(now,r->hcho_at)) {
        bool probe=r->hcho_probe<3;
        (void)send(r,M1_HCHO,probe?hcho_probe:hcho_query,9);
        if(probe) r->hcho_probe++;
        r->hcho_at=now+(probe?500:1000);
    }
    if(r->pm_probing && due(now,r->pm_probe_until)) r->pm_probing=false;
    if(due(now,r->pm_at)) {
        if(send(r,M1_PM,pm_probe,7) && r->pm_auto) {
            r->pm_probing=true; r->pm_probe_until=now+1000;
        }
        r->pm_at=now+10000;
        if(r->health[1].fault) r->pm_warmup=0;
        if(r->health[0].fault) r->hcho_probe=0;
    }
    for(unsigned i=0;i<3;++i) {
        m1_health *h=&r->health[i];
        if(now-h->last_good>6000 && !h->fault) {
            h->fault=true; h->timeouts++;
            if(i==0) { r->state.sample.hcho_health=0; r->hcho_probe=0; error(r,8); }
            if(i==1) { r->state.sample.pm25_health=0; r->pm_warmup=0; error(r,9); }
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
