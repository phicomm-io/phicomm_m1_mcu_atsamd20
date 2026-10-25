/* SPDX-License-Identifier: MIT */
#include "m1/runtime.h"
#include "m1/display.h"
#define LEASE_MS 10000u
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8|p[1]); }
static void put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static bool due(uint32_t now,uint32_t at) { return (int32_t)(now-at)>=0; }
static bool zero(const uint8_t *p,unsigned n)
{ for(unsigned i=0;i<n;++i) if(p[i]) return false; return true; }
static void stop_buzzer(m1_runtime *r)
{
    m1_music_stop(&r->music);
    r->music_applied=false;
    r->debug_buzzer=false;
    if(r->io.buzzer_pwm) (void)r->io.buzzer_pwm(r->io.context,2000,50);
    r->io.buzzer(r->io.context,false); r->buzzer_applied=false;
}
static uint8_t start_buzzer(m1_runtime *r,uint16_t hz,uint8_t duty,uint16_t ms,uint32_t now)
{
    if(!r->io.buzzer_pwm) return 3;
    m1_music_stop(&r->music);r->music_applied=false;
    if(!r->io.buzzer_pwm(r->io.context,hz,duty)) { stop_buzzer(r); return 4; }
    r->debug_buzzer=true; r->debug_buzzer_until=now+ms;
    r->io.buzzer(r->io.context,true);
    return 0;
}
void m1_debug_poll(m1_runtime *r,uint32_t now)
{
    if(r->debug_display && due(now,r->debug_display_until)) {
        r->debug_display=r->debug_raw=false; r->dirty=true;
    }
    if(r->debug_buzzer && due(now,r->debug_buzzer_until)) stop_buzzer(r);
}
static void screen(m1_runtime *r,uint32_t now)
{
    if(!r->debug_display) r->debug_state=r->state;
    r->debug_display=true; r->debug_display_until=now+LEASE_MS; r->dirty=true;
}
void m1_co2_poll(m1_runtime *r,uint32_t now)
{
    if(r->co2_valid && due(now,r->co2_until))r->co2_valid=false;
    bool show=r->co2_mode && r->co2_valid;
    if(show && r->co2_mode==2) {
        uint32_t period=(uint32_t)r->co2_interval*1000;
        uint32_t elapsed=now-r->co2_phase_at;
        if(elapsed>=period*2) { r->co2_phase_at+=(elapsed/(period*2))*(period*2);elapsed=now-r->co2_phase_at; }
        show=elapsed<period;
    }
    if(show!=r->co2_show){r->co2_show=show;r->dirty=true;}
}
bool m1_debug_command(m1_runtime *r,const uint8_t p[12],uint8_t out[20],uint32_t now)
{
    if(p[1]!=0x70) return false;
    m1_event(0x70,out); out[2]=p[2]; out[3]=p[3];
    const uint8_t *a=p+4;
    if(p[0]!=0x23 || p[11]!=0x21) { out[4]=1; return true; }
    m1_debug_poll(r,now);
    switch(p[2]) {
    case 0:
        if(!zero(a,7)) goto invalid;
        out[5]=2; put16(out+6,0xffff); put16(out+8,LEASE_MS);
        out[10]=r->io.buzzer_pwm!=0; break;
    case 1: {
        uint16_t hz=be16(a),ms=be16(a+3);
        if(!zero(a+5,2) || a[2]>100) goto invalid;
        if(!a[2]) { stop_buzzer(r); break; }
        if(hz<100 || hz>10000 || !ms || ms>LEASE_MS) goto invalid;
        out[4]=start_buzzer(r,hz,a[2],ms,now); break;
    }
    case 2: {
        uint16_t hcho=be16(a),pm=be16(a+2),bits=be16(a+4);
        int32_t t=bits>=0x8000u?(int32_t)bits-65536:bits;
        if(hcho>9999 || pm>999 || t < -4000 || t>12500 || a[6]>100) goto invalid;
        screen(r,now); r->debug_raw=false; r->debug_state.display_test=false;
        r->debug_state.hcho_raw=r->debug_state.sample.hcho=hcho;
        r->debug_state.sample.pm25=pm; r->debug_state.sample.temperature_centi_c=(int16_t)t;
        r->debug_state.sample.humidity_centi_pct=(uint16_t)(a[6]*100u); break;
    }
    case 3: {
        uint16_t ms=be16(a+3);
        if((a[0]!=0 && a[0]!=25 && a[0]!=50 && a[0]!=75 && a[0]!=100) ||
           a[1]>1 || a[2]>1 || !ms || ms>LEASE_MS || !zero(a+5,2)) goto invalid;
        screen(r,now); r->debug_state.brightness=a[0]; r->debug_state.display_on=a[1]!=0;
        r->debug_state.display_test=a[2]!=0; r->debug_raw=false;
        r->debug_display_until=now+ms; break;
    }
    case 4:
        if(a[1]<1 || a[1]>5 || a[0]>=M1_DISPLAY_BYTES || a[1]>M1_DISPLAY_BYTES-a[0] ||
           !zero(a+2+a[1],5-a[1])) goto invalid;
        screen(r,now);
        if(!r->debug_raw) m1_display_render(&r->debug_state,r->debug_ram);
        for(unsigned i=0;i<a[1];++i) r->debug_ram[a[0]+i]=a[2+i];
        r->debug_raw=true; break;
    case 5:
        if(a[0]>2 || !zero(a+1,6)) goto invalid;
        if(a[0]!=2) { r->debug_display=r->debug_raw=false; r->dirty=true; }
        if(a[0]!=1) stop_buzzer(r);
        break;
    case 6:
        if(!zero(a,7)) goto invalid;
        out[5]=r->pm_mode; out[6]=r->pm_warmup;
        out[7]=r->state.sample.hcho_health; out[8]=r->state.sample.pm25_health; out[9]=r->state.sample.climate_health;
        put16(out+10,r->climate_status); out[12]=r->climate_status_valid;
        put16(out+13,r->climate_resets); put16(out+15,(uint16_t)r->rx_errors); put16(out+17,(uint16_t)r->tx_errors); break;
    case 7:
        if((a[0]!=0 && a[0]!=2 && a[0]!=3) || !zero(a+1,6)) goto invalid;
        m1_runtime_pm_mode(r,a[0],now); break;
    case 8: {
        if(a[1]<1 || a[1]>12 || a[0]>=M1_DISPLAY_BYTES || a[1]>M1_DISPLAY_BYTES-a[0] || !zero(a+2,5)) goto invalid;
        uint8_t ram[M1_DISPLAY_BYTES];
        m1_runtime_display_ram(r,ram);
        out[5]=a[0]; out[6]=a[1]; for(unsigned i=0;i<a[1];++i) out[7+i]=ram[a[0]+i]; break;
    }
    case 9:
        if(!zero(a,7)) goto invalid;
        put16(out+5,r->state.hcho_threshold[0]); put16(out+7,r->state.hcho_threshold[1]);
        put16(out+9,r->state.pm25_threshold[0]); put16(out+11,r->state.pm25_threshold[1]);
        out[13]=3; /* bit 0: configurable lamps; bit 1: high-band alarm */
        break;
    case 10: {
        uint16_t seconds=be16(a+2);
        if(a[0]>2 || a[1]>1 || !seconds || seconds>3600 || !zero(a+4,3))goto invalid;
        if(r->co2_mode!=a[0] || r->co2_slot!=a[1] || r->co2_interval!=seconds)r->co2_phase_at=now;
        r->co2_mode=a[0];r->co2_slot=a[1];r->co2_interval=seconds;r->dirty=true;
        m1_co2_poll(r,now);break;
    }
    case 11: {
        uint16_t low=be16(a+1),high=be16(a+3);
        if(a[0]>2 || low>high || !zero(a+5,2))goto invalid;
        uint16_t *pair=a[0]==0?r->state.hcho_threshold:a[0]==1?r->state.pm25_threshold:r->co2_threshold;
        pair[0]=low;pair[1]=high;
        if(a[0]<2 && r->debug_display) {
            uint16_t *overlay=a[0]?r->debug_state.pm25_threshold:r->debug_state.hcho_threshold;
            overlay[0]=low;overlay[1]=high;
        }
        r->dirty=true;break;
    }
    case 12: {
        uint16_t ppm=be16(a),ttl=be16(a+3);
        if(a[2]>1 || !zero(a+5,2) || (a[2] && (!ppm || !ttl || ttl>60)))goto invalid;
        bool fresh=!r->co2_valid || due(now,r->co2_until);
        if(r->co2_ppm!=ppm || r->co2_valid!=(a[2]!=0))r->dirty=true;
        r->co2_ppm=ppm;r->co2_valid=a[2]!=0;r->co2_until=now+(uint32_t)ttl*1000;
        if(fresh && r->co2_valid)r->co2_phase_at=now;
        m1_co2_poll(r,now);break;
    }
    case 13:
        if(!zero(a,7))goto invalid;
        m1_co2_poll(r,now);
        out[5]=r->co2_mode;out[6]=r->co2_slot;put16(out+7,r->co2_interval);
        put16(out+9,r->co2_threshold[0]);put16(out+11,r->co2_threshold[1]);
        put16(out+13,r->co2_ppm);out[15]=r->co2_valid;out[16]=r->co2_show;
        put16(out+17,r->co2_valid?(uint16_t)((r->co2_until-now+999)/1000):0);break;
    case 14: {
        /* Each preset is a single uninterrupted tone, not a melody. ID 0
         * exits the override. Ordinary manual/alarm policy then resumes. */
        static const struct { uint16_t hz,ms; } tones[]={
            {0,0},{2000,60},{2500,120},{1500,250},{1000,500}
        };
        if(a[0]>=sizeof(tones)/sizeof(tones[0]) || !zero(a+1,6))goto invalid;
        if(!a[0]) { stop_buzzer(r); break; }
        out[4]=start_buzzer(r,tones[a[0]].hz,50,tones[a[0]].ms,now);
        if(!out[4]) {
            out[5]=a[0];put16(out+6,tones[a[0]].hz);put16(out+8,tones[a[0]].ms);out[10]=50;
        }
        break;
    }
    case 15:
        if(a[0]==5 && !r->io.buzzer_pwm){out[4]=3;break;}
        out[4]=m1_music_command(&r->music,&r->io.flash,a,out+5,now);
        if(!out[4] && a[0]==5){r->debug_buzzer=false;r->music_applied=true;}
        if(!out[4] && a[0]==6)stop_buzzer(r);
        break;
    default: out[4]=3; break;
    }
    return true;
invalid:
    out[4]=1; return true;
}
