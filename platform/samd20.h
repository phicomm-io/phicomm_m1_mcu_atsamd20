/* SPDX-License-Identifier: MIT */
#ifndef M1_SAMD20_H
#define M1_SAMD20_H
#include "m1/runtime.h"
bool m1_platform_clock(void);
bool m1_platform_init(m1_io *);
uint32_t m1_platform_millis(void);
bool m1_platform_button(void);
void m1_platform_drain(m1_runtime *);
void m1_platform_watchdog(void);
m1_flash m1_platform_flash(void);
void m1_platform_jump(void) __attribute__((noreturn));
#endif
