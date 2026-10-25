/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/pm_filter.h"

void m1_pm_filter_reset(m1_pm_filter *f)
{ *f=(m1_pm_filter){0}; }

static uint16_t rounded(const m1_pm_filter *f)
{ return (uint16_t)((f->q8+128u)>>8); }

void m1_pm_filter_interrupt(m1_pm_filter *f)
{
    if(!f->seeded) return;
    uint16_t value=rounded(f);
    for(unsigned i=0;i<3;++i) f->history[i]=value;
    f->next=0;
}

static uint16_t median(uint16_t a,uint16_t b,uint16_t c)
{
    if(a>b) { uint16_t t=a;a=b;b=t; }
    if(b>c) b=c;
    return a>b?a:b;
}

uint16_t m1_pm_filter_add(m1_pm_filter *f,uint16_t raw,bool *rejected)
{
    *rejected=false;
    if(!f->seeded) {
        for(unsigned i=0;i<3;++i) f->history[i]=raw;
        f->q8=(uint32_t)raw<<8;f->next=0;f->seeded=true;
        return raw;
    }
    f->history[f->next]=raw;f->next=(uint8_t)((f->next+1)%3);
    uint16_t value=median(f->history[0],f->history[1],f->history[2]);
    uint32_t distance=raw>value?(uint32_t)raw-value:(uint32_t)value-raw;
    *rejected=distance>=20u && distance*2u>value;

    /* A single spike or dip cannot move the median. After confirmation,
     * a rise >=20 ug/m3 follows by 75% per sample; smaller rises follow
     * by 50% and decreases by 25%. No floating point or heap allocation. */
    int32_t delta=(int32_t)((uint32_t)value<<8)-(int32_t)f->q8;
    int32_t step=delta>=20*256?(delta*3)/4:delta>0?delta/2:delta/4;
    f->q8=(uint32_t)((int32_t)f->q8+step);
    return rounded(f);
}
