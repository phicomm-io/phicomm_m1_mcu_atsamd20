/* SPDX-License-Identifier: MIT
 * Keep the complete portable core in the ARM link. No UART/display/sensor HAL.
 * This is not an operational firmware and must not be offered as an update. */
#include "m1/core.h"
volatile m1_state debug_state;
volatile m1_result debug_result;
volatile uint8_t debug_reply[20];
int main(void)
{
    m1_state s; m1_init(&s);
    uint8_t f[12] = {0x23, 0x11, 0,0,0,0,0,0,0,0,0,0x21}, reply[20];
    debug_result = m1_command(&s, f, reply);
    debug_state = s;
    for (unsigned i = 0; i < 20; ++i) debug_reply[i] = reply[i];
    for (;;) __asm volatile("wfi");
}
