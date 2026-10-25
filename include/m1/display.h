/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef M1_DISPLAY_H
#define M1_DISPLAY_H
#include "m1/core.h"
/* Complete stock display layout; no transport or physical color assumptions. */
void m1_display_render(const m1_state *state, uint8_t ram[M1_DISPLAY_BYTES]);
uint8_t m1_display_brightness(uint8_t percent);
/* slot 0: HCHO, 1: PM. Three digits; >=1000 uses kppm, truncated to .01k. */
void m1_display_co2(uint8_t ram[M1_DISPLAY_BYTES], uint8_t slot, uint16_t ppm,
                    const uint16_t thresholds[2]);
/* Three digits, decimal 0/1/2, unit 0=hidden/1=original glass legend and unit,
 * color 0=off/1=red/2=green/3=yellow. No gas conversion or threshold policy. */
void m1_display_external(uint8_t ram[M1_DISPLAY_BYTES],uint8_t slot,uint16_t number,
                         uint8_t format,uint8_t color);
#endif
