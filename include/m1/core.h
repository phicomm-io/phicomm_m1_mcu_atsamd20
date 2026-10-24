/* SPDX-License-Identifier: MIT */
#ifndef M1_CORE_H
#define M1_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M1_COMMAND_BYTES 12u
#define M1_REPLY_BYTES 20u
#define M1_DISPLAY_BYTES 29u
#define M1_VERSION "oss-0.1"

typedef struct {
    uint16_t hcho, pm25;
    int16_t temperature_centi_c;
    uint16_t humidity_centi_pct;
    uint8_t pm25_health, hcho_health, climate_health;
} m1_sample;

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} m1_clock;

typedef struct {
    m1_sample sample;
    m1_clock clock;
    uint16_t hcho_threshold[2], pm25_threshold[2];
    uint8_t brightness;
    bool display_on, buzzer_on;
} m1_state;

typedef struct {
    uint8_t bytes[M1_COMMAND_BYTES];
    size_t used;
    uint32_t discarded;
} m1_stream;

typedef enum { M1_BAD_FRAME, M1_UNSUPPORTED, M1_APPLIED, M1_REPLY } m1_result;

void m1_init(m1_state *state);
bool m1_feed(m1_stream *stream, uint8_t byte, uint8_t frame[M1_COMMAND_BYTES]);
m1_result m1_command(m1_state *state, const uint8_t frame[M1_COMMAND_BYTES],
                     uint8_t reply[M1_REPLY_BYTES]);
void m1_sample_reply(const m1_sample *sample, uint8_t reply[M1_REPLY_BYTES]);
void m1_button_short(m1_state *state, uint8_t reply[M1_REPLY_BYTES]);
void m1_network_reset_event(uint8_t reply[M1_REPLY_BYTES]);

/* TM1680 RAM offsets, relative to the stock buffer's first display byte.
 * Group names are numeric: physical colors have not been measured. */
unsigned m1_hcho_group(uint16_t raw);
unsigned m1_pm25_group(uint16_t raw);
void m1_display_groups(uint8_t ram[M1_DISPLAY_BYTES], unsigned hcho, unsigned pm25);

/* This is the recovered OTA bit-input CRC, not CRC-16/CCITT-FALSE. */
uint16_t m1_stock_crc_byte(uint16_t crc, uint8_t byte);
uint8_t m1_sht_crc(const uint8_t *data, size_t count);
bool m1_sht_raw(const uint8_t frame[6], uint16_t *temperature, uint16_t *humidity);
bool m1_hcho_raw(const uint8_t frame[9], uint16_t *value);
bool m1_pm25_raw(const uint8_t frame[32], uint16_t *value);
#endif
