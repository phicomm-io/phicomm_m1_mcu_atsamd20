/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/display.h"
/* Observed seven-segment encodings from stock 0x66a0; bit 3 is separate. */
static const uint8_t font[10] = {0xf5,0x05,0xb3,0x97,0x47,0xd6,0xf6,0x85,0xf7,0xd7};
static uint8_t glyph(unsigned n) { return font[n % 10]; }
static void put(uint8_t *ram, unsigned at, unsigned n) { ram[at] = (ram[at] & 8) | glyph(n); }
uint8_t m1_display_brightness(uint8_t percent)
{ return percent == 25 ? 0xb1 : percent == 50 ? 0xb7 : percent == 75 ? 0xbb : 0xbf; }
void m1_display_external(uint8_t ram[M1_DISPLAY_BYTES],uint8_t slot,uint16_t number,
                         uint8_t format,uint8_t color)
{
    unsigned left=slot?24:10,mid=slot?23:9,right=slot?22:4;
    uint8_t legend=slot?0x85:0x30,mask=slot?0x85:0x70;
    ram[21]&=(uint8_t)~legend;
    ram[left]&=(uint8_t)~8;ram[mid]&=(uint8_t)~8;ram[right]&=(uint8_t)~8;
    put(ram,left,number/100);put(ram,mid,number/10);put(ram,right,number);
    unsigned decimal=format&3;
    if(number<100 && decimal!=1)ram[left]=0;
    if(number<10 && !decimal)ram[mid]=0;
    if(decimal==1)ram[left]|=8;
    if(decimal==2)ram[mid]|=8;
    if(format&4){ram[21]|=legend;ram[right]|=8;}
    const unsigned offsets[3]={12,19,18};
    for(unsigned i=0;i<3;i++)ram[offsets[i]]&=(uint8_t)~mask;
    if(color)ram[color==M1_GROUP_RED?12:color==M1_GROUP_GREEN?19:18]|=mask;
}
void m1_display_formatted(uint8_t ram[M1_DISPLAY_BYTES],uint8_t slot,uint16_t number,
                          uint8_t format,uint16_t lamps)
{
    unsigned left=slot?24:10,mid=slot?23:9,right=slot?22:4;
    uint8_t legend=slot?0x85:0x30,mask=slot?0x85:0x70;
    ram[21]&=(uint8_t)~legend;
    ram[left]&=(uint8_t)~8;ram[mid]&=(uint8_t)~8;ram[right]&=(uint8_t)~8;
    put(ram,left,number/100);put(ram,mid,number/10);put(ram,right,number);
    if(format&M1_FORMATTED_DECIMAL)ram[left]|=8;
    else {
        if(number<100)ram[left]=0;
        if(number<10)ram[mid]=0;
    }
    if(format&M1_FORMATTED_LABEL)ram[21]|=legend;
    if(format&M1_FORMATTED_UNIT)ram[right]|=8;
    static const uint8_t offsets[3]={12,19,18};
    static const uint8_t positions[2][3]={{0x10,0x20,0x40},{0x80,0x01,0x04}};
    for(unsigned color=0;color<3;color++) {
        ram[offsets[color]]&=(uint8_t)~mask;
        for(unsigned position=0;position<3;position++)
            if(lamps&(1u<<(color*3+position)))ram[offsets[color]]|=positions[slot][position];
    }
}
void m1_display_co2(uint8_t ram[M1_DISPLAY_BYTES],uint8_t slot,uint16_t ppm,const uint16_t thresholds[2])
{
    unsigned left=slot?24:10,mid=slot?23:9,right=slot?22:4;
    /* The stock HCHO/PM routines set these legend bits and the right unit bit.
     * Keep all unrelated date/time/climate segments intact. No CO2/k legend
     * exists on the glass: the decimal distinguishes ppm from kppm. */
    ram[21]&=(uint8_t)~(slot?0x85:0x30);
    ram[left]&=(uint8_t)~8; ram[mid]&=(uint8_t)~8; ram[right]&=(uint8_t)~8;
    unsigned n=ppm;
    if(n>=10000) { n=n/100;ram[mid]|=8; }
    else if(n>=1000) { n=n/10;ram[left]|=8; }
    put(ram,left,n/100);put(ram,mid,n/10);put(ram,right,n);
    if(ppm<100)ram[left]&=8;
    if(ppm<10)ram[mid]&=8;
    unsigned group=m1_threshold_group(ppm,thresholds);
    const unsigned offsets[3]={12,19,18};
    uint8_t mask=slot?0x85:0x70;
    for(unsigned i=0;i<3;i++)ram[offsets[i]]&=(uint8_t)~mask;
    ram[group==M1_GROUP_RED?12:group==M1_GROUP_GREEN?19:18]|=mask;
}
void m1_display_render(const m1_state *s, uint8_t ram[M1_DISPLAY_BYTES])
{
    for (unsigned i=0; i<M1_DISPLAY_BYTES; ++i) ram[i] = s->display_test ? 0xff : 0;
    if (s->display_test) return;
    unsigned year=s->clock.year, month=s->clock.month, day=s->clock.day;
    put(ram,28,year/1000); put(ram,27,year/100); put(ram,26,year/10); put(ram,25,year);
    put(ram,20,month/10); put(ram,17,month); put(ram,13,day/10); put(ram,11,day);
    ram[25]|=8; ram[17]|=8;
    put(ram,1,s->clock.hour/10); put(ram,0,s->clock.hour);
    put(ram,3,s->clock.minute/10); put(ram,2,s->clock.minute); ram[0]|=8; ram[3]|=8;
    unsigned h=s->hcho_raw>9999?9990:s->sample.hcho;
    if (h>9999) h=9990;
    put(ram,10,h/1000); put(ram,9,h/100); put(ram,4,h/10); ram[10]|=8; ram[4]|=8;
    unsigned pm=s->sample.pm25;
    if (pm>999) pm=999;
    put(ram,24,pm/100); put(ram,23,pm/10); put(ram,22,pm); ram[22]|=8;
    if (pm<100) ram[24]&=8;
    if (pm<10) ram[23]&=8;
    int32_t t=s->sample.temperature_centi_c;
    bool negative=t<0; if (negative) t=-t;
    unsigned integral=(unsigned)t/100, tenth=(unsigned)t/10%10;
    if (negative) {
        ram[16]=2; /* negative sign occupies left digit */
        put(ram,15,integral>9 ? 9 : integral); put(ram,14,integral>9 ? 9 : tenth);
    } else {
        put(ram,16,integral/10); put(ram,15,integral); put(ram,14,tenth);
    }
    ram[15]|=8; ram[14]|=8; ram[8]|=0x81;
    unsigned rh=s->sample.humidity_centi_pct;
    if (rh>=10000) rh=9990;
    put(ram,7,rh/1000); put(ram,6,rh/100); put(ram,5,rh/10); ram[6]|=8; ram[5]|=8;
    ram[8]|=0x74; ram[21]|=0xf7;
    m1_display_groups(ram,m1_threshold_group(s->hcho_raw,s->hcho_threshold),
                      m1_threshold_group(s->sample.pm25,s->pm25_threshold));
}
