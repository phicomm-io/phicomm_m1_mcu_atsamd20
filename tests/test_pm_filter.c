/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/pm_filter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint16_t feed(m1_pm_filter *f,uint16_t raw)
{ bool rejected;return m1_pm_filter_add(f,raw,&rejected); }

static void excursions(void)
{
    const uint16_t bases[]={0,1,30,75,150,1000,UINT16_MAX};
    for(unsigned i=0;i<sizeof bases/sizeof *bases;++i) {
        m1_pm_filter f={0};uint16_t base=bases[i];
        assert(feed(&f,base)==base);
        /* Every excursion is followed by actual baseline samples: neither
         * a single high frame nor a single zero moves the display. */
        for(unsigned j=0;j<100;++j) {
            assert(feed(&f,UINT16_MAX)==base);
            assert(feed(&f,base)==base);assert(feed(&f,base)==base);
            assert(feed(&f,0)==base);
            assert(feed(&f,base)==base);assert(feed(&f,base)==base);
        }
    }
    m1_pm_filter f={0};bool rejected;
    assert(feed(&f,30)==30);
    assert(m1_pm_filter_add(&f,1200,&rejected)==30 && rejected);
    assert(m1_pm_filter_add(&f,30,&rejected)==30 && !rejected);
    assert(m1_pm_filter_add(&f,30,&rejected)==30 && !rejected);
    /* Raw detail availability remains valid during ordinary small jitter. */
    (void)m1_pm_filter_add(&f,33,&rejected);assert(!rejected);
}

static void steps(void)
{
    m1_pm_filter f={0};assert(feed(&f,30)==30);
    assert(feed(&f,300)==30);
    uint16_t up=feed(&f,300);
    assert(up>=230 && up<=300); /* >=74% of rise at confirmation (3 s) */
    up=feed(&f,300);assert(up>=280 && up<=300); /* >92% after 6 s */
    for(unsigned i=0;i<30;++i) {
        uint16_t next=feed(&f,300);assert(next>=up && next<=300);up=next;
    }
    assert(up==300);
    assert(feed(&f,30)==300);
    uint16_t down=feed(&f,30);assert(down>200 && down<300);
    for(unsigned i=0;i<40;++i) {
        uint16_t next=feed(&f,30);assert(next<=down && next>=30);down=next;
        if(i==8) assert(down<=50); /* >92% of fall in 30 s */
    }
    assert(down==30);
    m1_pm_filter_reset(&f);assert(!f.seeded && feed(&f,0)==0);
    assert(feed(&f,10)==0);
    assert(feed(&f,10)>=5); /* small sustained rises are not a deadband */
    for(unsigned i=0;i<30;++i) (void)feed(&f,10);
    assert(feed(&f,10)==10);
}

static void jitter(void)
{
    m1_pm_filter f={0};(void)feed(&f,75);
    const uint16_t noise[]={75,83,66,78,70,84,72,68,80,74,82,67};
    uint64_t raw_energy=0,filtered_energy=0;unsigned raw_travel=0,filtered_travel=0;
    int previous_raw=75,previous_filtered=75;
    for(unsigned i=0;i<240;++i) {
        int raw=noise[i%(sizeof noise/sizeof *noise)],value=feed(&f,(uint16_t)raw);
        raw_energy+=(raw-75)*(raw-75);filtered_energy+=(value-75)*(value-75);
        raw_travel+=(unsigned)abs(raw-previous_raw);
        filtered_travel+=(unsigned)abs(value-previous_filtered);
        previous_raw=raw;previous_filtered=value;
        assert(value>=66 && value<=84);
    }
    assert(filtered_energy*4<raw_energy);
    assert(filtered_travel*3<raw_travel);
    printf("PM jitter: raw/filtered squared error=%llu/%llu, travel=%u/%u\n",
           (unsigned long long)raw_energy,(unsigned long long)filtered_energy,raw_travel,filtered_travel);
}

static void interruptions(void)
{
    m1_pm_filter f={0};(void)feed(&f,30);assert(feed(&f,300)==30);
    m1_pm_filter_interrupt(&f);
    assert(feed(&f,300)==30); /* bad frame breaks consecutive confirmation */
    assert(feed(&f,300)>200);
    m1_pm_filter_reset(&f);assert(feed(&f,7)==7);
}

static void bounds(void)
{
    m1_pm_filter f={0};uint32_t seed=42;uint16_t lo=UINT16_MAX,hi=0;
    for(unsigned i=0;i<100000;++i) {
        seed=seed*1664525u+1013904223u;uint16_t raw=(uint16_t)(seed>>16);
        if(raw<lo)lo=raw;
        if(raw>hi)hi=raw;
        uint16_t value=feed(&f,raw);assert(value>=lo && value<=hi);
        assert(f.q8<=((uint32_t)UINT16_MAX<<8) && f.next<3);
        if(i%317==0) m1_pm_filter_interrupt(&f);
        if(i%997==0) { m1_pm_filter_reset(&f);lo=UINT16_MAX;hi=0; }
    }
    for(unsigned a=0;a<2;++a) {
        m1_pm_filter_reset(&f);uint16_t start=a?UINT16_MAX:0,end=a?0:UINT16_MAX;
        (void)feed(&f,start);
        for(unsigned i=0;i<200;++i) (void)feed(&f,end);
        assert(feed(&f,end)==end);
    }
}

int main(void)
{ excursions();steps();jitter();interruptions();bounds();puts("PM filter tests passed"); }
