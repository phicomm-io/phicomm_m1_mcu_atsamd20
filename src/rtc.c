/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m1/core.h"
static unsigned unbcd(uint8_t b) { return (b >> 4) * 10u + (b & 15u); }
static uint8_t bcd(unsigned n) { return (uint8_t)((n / 10u) * 16u + n % 10u); }
static bool digit(uint8_t b) { return (b & 15u) < 10 && (b >> 4) < 10; }
bool m1_rtc_decode(const uint8_t r[8], m1_clock *clock)
{
    if (r[0] & 0x80 || r[2] & 0x80) return false; /* stopped or 12-hour mode */
    const uint8_t mask[7] = {0x7f,0x7f,0x3f,0x3f,0x1f,0x07,0xff};
    for (unsigned i = 0; i < 7; ++i) if (!digit(r[i] & mask[i])) return false;
    m1_clock c = {(uint16_t)(2000 + unbcd(r[6])),(uint8_t)unbcd(r[4] & 31),
        (uint8_t)unbcd(r[3] & 63),(uint8_t)unbcd(r[2] & 63),
        (uint8_t)unbcd(r[1] & 127),(uint8_t)unbcd(r[0] & 127)};
    if (!m1_clock_valid(&c)) return false;
    *clock = c; return true;
}
bool m1_rtc_encode(const m1_clock *c, uint8_t r[8])
{
    if (!m1_clock_valid(c)) return false;
    r[0]=bcd(c->second); r[1]=bcd(c->minute); r[2]=bcd(c->hour);
    r[3]=bcd(c->day); r[4]=bcd(c->month); r[5]=1; r[6]=bcd(c->year-2000); r[7]=0x80;
    return true;
}
void m1_clock_second(m1_clock *c)
{
    if (!m1_clock_valid(c)) return;
    if (++c->second < 60) return;
    c->second=0; if (++c->minute < 60) return;
    c->minute=0; if (++c->hour < 24) return;
    c->hour=0; c->day++;
    if (m1_clock_valid(c)) return;
    c->day=1; if (++c->month <= 12) return;
    c->month=1; c->year = c->year == 2099 ? 2000 : c->year+1;
}
