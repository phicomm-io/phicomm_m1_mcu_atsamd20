/* SPDX-License-Identifier: MIT */
#ifndef M1_DISPLAY_H
#define M1_DISPLAY_H
#include "m1/core.h"
/* Complete stock display layout; no transport or physical color assumptions. */
void m1_display_render(const m1_state *state, uint8_t ram[M1_DISPLAY_BYTES]);
uint8_t m1_display_brightness(uint8_t percent);
#endif
