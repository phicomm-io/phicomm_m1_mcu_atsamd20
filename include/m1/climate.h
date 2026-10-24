/* SPDX-License-Identifier: MIT */
#ifndef M1_CLIMATE_H
#define M1_CLIMATE_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { int64_t x[4], heat, aux; bool initialized; } m1_climate;
/* Recovered fixed-point model. Outputs milli-degrees and milli-percent. */
bool m1_climate_convert(m1_climate *model, uint16_t raw_t, uint16_t raw_h,
                        uint32_t auxiliary, bool display_on, int32_t *t, uint32_t *rh);
#endif
