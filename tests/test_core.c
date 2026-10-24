/* SPDX-License-Identifier: MIT */
#include "m1/core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void frame(uint8_t *f, uint8_t cmd)
{ memset(f, 0, 12); f[0] = 0x23; f[1] = cmd; f[11] = 0x21; }

static void protocol(void)
{
    m1_state s; m1_init(&s);
    uint8_t f[12], r[20]; memset(r, 0xa5, sizeof(r));
    frame(f, 1);
    s.sample = (m1_sample){.hcho=1234, .pm25=432, .temperature_centi_c=-550,
        .humidity_centi_pct=4567, .pm25_health=1, .hcho_health=2, .climate_health=1};
    assert(m1_command(&s, f, r) == M1_REPLY);
    const uint8_t expected[20] = {0x23,1,4,0xd2,1,5,50,45,67,1,0xb0,1,2,1,0,0,0,0,0,0x21};
    assert(!memcmp(r, expected, 20));
    s.sample.temperature_centi_c = -1;
    m1_sample_reply(&s.sample, r);
    assert(r[4] == 1 && r[5] == 0 && r[6] == 1);
    frame(f, 0x11); assert(m1_command(&s, f, r) == M1_REPLY);
    assert(r[2] == strlen(M1_VERSION) && !memcmp(r + 3, M1_VERSION, r[2]));
    assert(r[19] == 0x21 && r[18] == 0);
    frame(f, 2); f[2] = 50; f[3] = 0;
    assert(m1_command(&s, f, r) == M1_APPLIED && !s.display_on && s.brightness == 50);
    f[2] = 255; assert(m1_command(&s, f, r) == M1_BAD_FRAME && s.brightness == 50);
    frame(f, 5); f[3] = 100; f[4] = 1; f[5] = 44;
    assert(m1_command(&s, f, r) == M1_APPLIED);
    assert(s.hcho_threshold[0] == 100 && s.hcho_threshold[1] == 300);
    assert(s.pm25_threshold[0] == 0);
    f[3] = 0; f[4] = 0; f[5] = 0;
    assert(m1_command(&s, f, r) == M1_BAD_FRAME && s.hcho_threshold[0] == 100);
    frame(f, 0x0e); f[2] = 1;
    assert(m1_command(&s, f, r) == M1_APPLIED && s.buzzer_on);
    f[2] = 0; assert(m1_command(&s, f, r) == M1_APPLIED && !s.buzzer_on);
    frame(f, 3); f[2] = 7; f[3] = 0xe8; f[4] = 2; f[5] = 29;
    f[6] = 23; f[7] = 59; f[8] = 59;
    assert(m1_command(&s, f, r) == M1_APPLIED && s.clock.year == 2024);
    f[3] = 0xe9; assert(m1_command(&s, f, r) == M1_BAD_FRAME && s.clock.year == 2024);
    frame(f, 0x0d); assert(m1_command(&s, f, r) == M1_REPLY && r[5] == 29);
    for (unsigned type = 0x12; type <= 0x17; ++type) {
        frame(f, (uint8_t)type); m1_state before = s;
        assert(m1_command(&s, f, r) == M1_UNSUPPORTED);
        assert(!memcmp(&s, &before, sizeof(s)));
    }
    frame(f, 1); f[11] = 0;
    assert(m1_command(&s, f, r) == M1_BAD_FRAME);
    m1_button_short(&s, r); assert(s.display_on && r[1] == 0x0f && r[2] == 1);
    m1_network_reset_event(r); assert(r[1] == 0x18 && r[19] == 0x21);
}

static void stream(void)
{
    m1_stream s = {0}; uint8_t f[12], out[12]; frame(f, 0x11);
    assert(!m1_feed(&s, 0, out));
    /* Truncated packet followed immediately by a complete packet. */
    for (unsigned i = 0; i < 4; ++i) assert(!m1_feed(&s, f[i], out));
    unsigned count = 0;
    for (unsigned i = 0; i < 12; ++i) count += m1_feed(&s, f[i], out);
    assert(count == 1 && !memcmp(out, f, 12) && s.discarded == 5);
    for (unsigned n = 0; n < 3; ++n)
        for (unsigned i = 0; i < 12; ++i) assert(m1_feed(&s, f[i], out) == (i == 11));
}

static void sensors(void)
{
    uint8_t sht[6] = {0xbe,0xef,0x92,0x12,0x34,0}; uint16_t t=0,h=0,v=0;
    assert(m1_sht_crc(sht, 2) == 0x92);
    sht[5] = m1_sht_crc(sht + 3, 2);
    assert(m1_sht_raw(sht, &t, &h) && t == 0xbeef && h == 0x1234);
    sht[2] ^= 1; t = 11; assert(!m1_sht_raw(sht, &t, &h) && t == 11);
    uint8_t jq[9] = {0xff,0x86,1,44,0,0,0,0,0};
    for (unsigned i = 1; i < 8; ++i) jq[8] = (uint8_t)(jq[8] - jq[i]);
    assert(m1_hcho_raw(jq, &v) && v == 300);
    jq[8] ^= 1; assert(!m1_hcho_raw(jq, &v));
    uint8_t pm[32] = {0x42,0x4d,0,28}; pm[12] = 1; pm[13] = 44;
    unsigned sum = 0; for (unsigned i = 0; i < 30; ++i) sum += pm[i];
    pm[30] = (uint8_t)(sum >> 8); pm[31] = (uint8_t)sum;
    assert(m1_pm25_raw(pm, &v) && v == 300);
    pm[29] = 1; assert(!m1_pm25_raw(pm, &v));
    pm[29] = 0; pm[31] ^= 1; assert(!m1_pm25_raw(pm, &v));
}

static void display(void)
{
    assert(m1_hcho_group(100) == 2 && m1_hcho_group(101) == 3);
    assert(m1_hcho_group(300) == 3 && m1_hcho_group(301) == 1);
    assert(m1_pm25_group(75) == 2 && m1_pm25_group(76) == 3);
    assert(m1_pm25_group(150) == 3 && m1_pm25_group(151) == 1);
    uint8_t ram[29]; memset(ram, 0x08, sizeof(ram));
    m1_display_groups(ram, 2, 1);
    assert(ram[0x13] == 0x78 && ram[0x0c] == 0x8d && ram[0x12] == 8);
    for (unsigned i = 0; i < 29; ++i)
        if (i != 0x13 && i != 0x0c && i != 0x12) assert(ram[i] == 8);
    m1_display_groups(ram, 3, 2);
    assert(ram[0x12] == 0x78 && ram[0x13] == 0x8d && ram[0x0c] == 8);
}

int main(void)
{
    protocol(); stream(); sensors(); display();
    assert(m1_stock_crc_byte(0, 0xff) == 0xff);
    puts("PASS: protocol, framing recovery, calendar, sensors, display boundaries");
    return 0;
}
