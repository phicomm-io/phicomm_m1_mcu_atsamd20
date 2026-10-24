/* SPDX-License-Identifier: MIT */
#ifndef M1_RUNTIME_H
#define M1_RUNTIME_H
#include "m1/core.h"
#include "m1/climate.h"
#include "m1/update.h"
typedef enum { M1_LINK, M1_HCHO, M1_PM } m1_channel;
typedef struct {
    void *context;
    bool (*uart)(void *,m1_channel,const uint8_t *,size_t);
    bool (*i2c)(void *,unsigned bus,uint8_t address,const uint8_t *,size_t,uint8_t *,size_t);
    bool (*rtc_read)(void *,m1_clock *);
    bool (*rtc_write)(void *,const m1_clock *);
    void (*buzzer)(void *,bool);
    void (*reset)(void *);
    /* Optional board controls; PWM availability is in debug capabilities. */
    void (*wifi_enable)(void *,bool);
    bool (*buzzer_pwm)(void *,uint16_t hz,uint8_t duty);
    m1_flash flash;
} m1_io;
typedef struct { uint8_t bytes[262]; uint16_t used; uint32_t last; } m1_rx;
typedef struct { uint32_t last_good,errors,timeouts; uint8_t bad; bool seen,fault; } m1_health;
typedef struct {
    m1_state state;
    m1_io io;
    m1_rx rx[3];
    m1_health health[3]; /* HCHO, PM, climate */
    m1_climate climate;
    m1_update update;
    uint32_t second_at,display_at,climate_at,hcho_at,pm_at,reset_at;
    uint32_t button_at,edge_at,buzzer_at,rx_errors,tx_errors,display_errors,rtc_errors;
    uint32_t display_retry,pm_probe_until,climate_reset_at;
    uint32_t debug_display_until,debug_buzzer_until;
    m1_state debug_state;
    uint8_t debug_ram[M1_DISPLAY_BYTES];
    uint16_t climate_status,climate_resets;
    uint16_t raw_t,raw_h,pm_previous;
    uint8_t hcho_probe,pm_warmup,climate_phase,buzzer_mode;
    uint8_t brightness_applied,pm_mode,reset_phase;
    bool pm_probing,pm_auto,climate_status_valid,climate_reset_due;
    bool debug_display,debug_raw,debug_buzzer;
    bool button_raw,button_down,button_3s,button_10s,pm_jump;
    bool display_ready,display_applied,dirty,reset_pending,low_hcho_seen;
    bool alarm_latched,buzzer_applied;
} m1_runtime;
typedef enum { M1_BUZZER_OFF, M1_BUZZER_CONTINUOUS, M1_BUZZER_CHIRP, M1_BUZZER_ALARM } m1_buzzer_mode;
void m1_runtime_pm_mode(m1_runtime *,uint8_t mode,uint32_t now);
bool m1_debug_command(m1_runtime *,const uint8_t frame[12],uint8_t reply[20],uint32_t now);
void m1_debug_poll(m1_runtime *,uint32_t now);
void m1_runtime_beep(m1_runtime *,m1_buzzer_mode,uint32_t now);
void m1_runtime_init(m1_runtime *,const m1_io *,uint32_t now);
void m1_runtime_rx(m1_runtime *,m1_channel,uint8_t byte,uint32_t now);
void m1_runtime_poll(m1_runtime *,uint32_t now,bool button_pressed);
void m1_runtime_rx_error(m1_runtime *,m1_channel);
#endif
