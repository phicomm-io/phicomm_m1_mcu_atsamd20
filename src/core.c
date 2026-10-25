/* SPDX-License-Identifier: MIT */
#include "m1/core.h"

static void zero(uint8_t *p, size_t n) { while (n--) *p++ = 0; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value; }
static void envelope(uint8_t *reply, uint8_t command)
{
    zero(reply, M1_REPLY_BYTES);
    reply[0] = 0x23; reply[1] = command; reply[19] = 0x21;
}

void m1_init(m1_state *state)
{
    *state = (m1_state){0};
    state->brightness = 100;
    state->display_on = true;
    state->hcho_threshold[0] = 100; state->hcho_threshold[1] = 300;
    state->pm25_threshold[0] = 75; state->pm25_threshold[1] = 150;
    /* Health remains zero until a real sensor driver publishes a sample. */
}

bool m1_feed(m1_stream *s, uint8_t byte, uint8_t frame[M1_COMMAND_BYTES])
{
    if (!s->used && byte != 0x23) { s->discarded++; return false; }
    s->bytes[s->used++] = byte;
    if (s->used != M1_COMMAND_BYTES) return false;
    if (s->bytes[11] == 0x21) {
        for (size_t i = 0; i < M1_COMMAND_BYTES; ++i) frame[i] = s->bytes[i];
        s->used = 0;
        return true;
    }
    size_t skip = 1;
    while (skip < M1_COMMAND_BYTES && s->bytes[skip] != 0x23) skip++;
    s->used -= skip;
    s->discarded += (uint32_t)skip;
    for (size_t i = 0; i < s->used; ++i) s->bytes[i] = s->bytes[i + skip];
    return false;
}

void m1_sample_reply(const m1_sample *s, uint8_t reply[M1_REPLY_BYTES])
{
    envelope(reply, 1);
    put16(reply + 2, s->hcho);
    int32_t t = s->temperature_centi_c;
    if (t < 0) { reply[4] = 1; t = -t; }
    reply[5] = (uint8_t)(t / 100);
    reply[6] = (uint8_t)(t % 100);
    reply[7] = (uint8_t)(s->humidity_centi_pct / 100u);
    reply[8] = (uint8_t)(s->humidity_centi_pct % 100u);
    put16(reply + 9, s->pm25);
    reply[11] = s->pm25_health; reply[12] = s->hcho_health; reply[13] = s->climate_health;
}

bool m1_clock_valid(const m1_clock *c)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (c->year < 2000 || c->year > 2099 || !c->month || c->month > 12 ||
        !c->day || c->hour > 23 || c->minute > 59 || c->second > 59) return false;
    unsigned max = days[c->month - 1];
    if (c->month == 2 && c->year % 4u == 0) max++;
    return c->day <= max;
}

m1_result m1_command(m1_state *s, const uint8_t f[M1_COMMAND_BYTES], uint8_t reply[M1_REPLY_BYTES])
{
    if (f[0] != 0x23 || f[11] != 0x21) return M1_BAD_FRAME;
    switch (f[1]) {
    case 1: m1_sample_reply(&s->sample, reply); return M1_REPLY;
    case 2: case 0x10:
        /* Deliberately validate instead of repeating the stock permissive path. */
        if (f[2] > 100 || f[2] % 25 || f[3] > 2) return M1_BAD_FRAME;
        s->brightness = f[2];
        s->display_on = f[3] == 2 ? !s->display_on : f[3] != 0;
        s->display_test = f[1] == 0x10;
        return M1_APPLIED;
    case 3: {
        m1_clock c = {be16(f + 2), f[4], f[5], f[6], f[7], f[8]};
        if (!m1_clock_valid(&c)) return M1_BAD_FRAME;
        s->clock = c; return M1_APPLIED;
    }
    case 5: case 6: {
        uint16_t a = be16(f + 2), b = be16(f + 4);
        if (a > b) return M1_BAD_FRAME;
        uint16_t *p = f[1] == 5 ? s->hcho_threshold : s->pm25_threshold;
        p[0] = a; p[1] = b;
        /* Original write format; configurable colors/alarm are OSS extensions.
         * Equality collapses the yellow band; zero is a value, not disable. */
        return M1_APPLIED;
    }
    case 0x0d:
        envelope(reply, 0x0d); put16(reply + 2, s->clock.year);
        reply[4] = s->clock.month; reply[5] = s->clock.day;
        reply[6] = s->clock.hour; reply[7] = s->clock.minute; reply[8] = s->clock.second;
        return M1_REPLY;
    case 0x0e: s->buzzer_on = f[2] != 0; return M1_APPLIED;
    case 0x11: {
        static const char version[] = M1_UART_VERSION;
        _Static_assert(sizeof(version) - 1 <= 16, "version exceeds UART payload");
        envelope(reply, 0x11); reply[2] = sizeof(version) - 1;
        for (size_t i = 0; i < sizeof(version) - 1; ++i) reply[3 + i] = (uint8_t)version[i];
        return M1_REPLY;
    }
    case 0x19:
        envelope(reply, 0x19); put16(reply + 2, s->hcho_raw); return M1_REPLY;
    /* Variable-length OTA transactions are handled by runtime.c/update.c. */
    default: return M1_UNSUPPORTED;
    }
}

void m1_button_short(m1_state *s, uint8_t reply[M1_REPLY_BYTES])
{ s->display_on = !s->display_on; envelope(reply, 0x0f); reply[2] = s->display_on; }
void m1_network_reset_event(uint8_t reply[M1_REPLY_BYTES]) { envelope(reply, 0x18); }
void m1_event(uint8_t command, uint8_t reply[M1_REPLY_BYTES]) { envelope(reply, command); }
void m1_error_event(uint8_t command, uint8_t reply[M1_REPLY_BYTES])
{
    envelope(reply, command);
    static const char text[] = "error";
    for (unsigned i = 0; i < 5; ++i) reply[i + 2] = (uint8_t)text[i];
    reply[7] = command == 7 ? 'u' : command == 8 || command == 10 ? 'j' : 'p';
    reply[8] = command == 7 ? 'p' : command == 8 || command == 10 ? 'q' : 'm';
    reply[9] = reply[10] = command >= 10 ? 0xee : 0xff;
}

unsigned m1_hcho_group(uint16_t value) { return value <= 100 ? 2u : value <= 300 ? 3u : 1u; }
unsigned m1_pm25_group(uint16_t value) { return value <= 75 ? 2u : value <= 150 ? 3u : 1u; }
unsigned m1_threshold_group(uint16_t value, const uint16_t thresholds[2])
{ return value <= thresholds[0] ? M1_GROUP_GREEN : value <= thresholds[1] ? M1_GROUP_YELLOW : M1_GROUP_RED; }
static void group(uint8_t *ram, unsigned g, uint8_t mask)
{
    static const uint8_t offsets[] = {0x0c, 0x13, 0x12};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *p = ram + offsets[i];
        *p &= (uint8_t)~mask;
        if (g == i + 1 || g == 9) *p |= mask;
    }
}
void m1_display_groups(uint8_t ram[M1_DISPLAY_BYTES], unsigned hcho, unsigned pm25)
{ group(ram, hcho, 0x70); group(ram, pm25, 0x85); }

uint16_t m1_stock_crc_byte(uint16_t crc, uint8_t byte)
{
    for (unsigned i = 0; i < 8; ++i) {
        bool top = (crc & 0x8000u) != 0;
        crc = (uint16_t)((uint16_t)(crc << 1) | (byte >> 7));
        if (top) crc ^= 0x1021;
        byte = (uint8_t)(byte << 1);
    }
    return crc;
}

uint8_t m1_sht_crc(const uint8_t *data, size_t count)
{
    uint8_t crc = 0xff;
    while (count--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x31 : 0));
    }
    return crc;
}
bool m1_sht_raw(const uint8_t f[6], uint16_t *t, uint16_t *h)
{
    if (m1_sht_crc(f, 2) != f[2] || m1_sht_crc(f + 3, 2) != f[5]) return false;
    *t = be16(f); *h = be16(f + 3); return true;
}
bool m1_hcho_raw(const uint8_t f[9], uint16_t *value)
{
    if (f[0] != 0xff || f[1] != 0x86) return false;
    uint8_t sum = 0;
    for (unsigned i = 1; i < 9; ++i) sum = (uint8_t)(sum + f[i]);
    if (sum) return false;
    *value = be16(f + 2); return true;
}
bool m1_pm25_raw(const uint8_t f[32], uint16_t *value)
{
    if (f[0] != 0x42 || f[1] != 0x4d || be16(f + 2) != 28 || f[29]) return false;
    uint16_t sum = 0;
    for (unsigned i = 0; i < 30; ++i) sum = (uint16_t)(sum + f[i]);
    if (sum != be16(f + 30)) return false;
    *value = be16(f + 12); return true;
}
