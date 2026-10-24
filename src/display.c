/* SPDX-License-Identifier: MIT */
#include "m1/display.h"
/* Observed seven-segment encodings from stock 0x66a0; bit 3 is separate. */
static const uint8_t font[10] = {0xf5,0x05,0xb3,0x97,0x47,0xd6,0xf6,0x85,0xf7,0xd7};
static uint8_t glyph(unsigned n) { return font[n % 10]; }
static void put(uint8_t *ram, unsigned at, unsigned n) { ram[at] = (ram[at] & 8) | glyph(n); }
uint8_t m1_display_brightness(uint8_t percent)
{ return percent == 25 ? 0xb1 : percent == 50 ? 0xb7 : percent == 75 ? 0xbb : 0xbf; }
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
    m1_display_groups(ram,m1_hcho_group(s->hcho_raw),m1_pm25_group(s->sample.pm25));
}
