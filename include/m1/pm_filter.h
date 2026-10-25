/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef M1_PM_FILTER_H
#define M1_PM_FILTER_H
#include <stdbool.h>
#include <stdint.h>

/* Three acquisitions, not three UART frames from the same acquisition.
 * The accumulator retains fractional precision at low concentrations. */
typedef struct {
    uint32_t q8;
    uint16_t history[3];
    uint8_t next;
    bool seeded;
} m1_pm_filter;

void m1_pm_filter_reset(m1_pm_filter *);
/* Break change confirmation after a bad frame without inventing a zero. */
void m1_pm_filter_interrupt(m1_pm_filter *);
/* Call only for a healthy, warmed-up, checksum-valid acquisition.
 * rejected identifies a large raw excursion excluded by the median. */
uint16_t m1_pm_filter_add(m1_pm_filter *,uint16_t raw,bool *rejected);
#endif
