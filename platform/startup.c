/* SPDX-License-Identifier: MIT
 * Layout/link check only: peripheral initialization is not implemented. */
#include <stdint.h>
extern uint32_t _stack_top, _data_start, _data_end, _data_load, _bss_start, _bss_end;
extern int main(void);
void Reset_Handler(void);
static void Default_Handler(void) { for (;;) __asm volatile("wfi"); }
__attribute__((section(".vectors"), used))
const uintptr_t vectors[41] = {
    [0] = (uintptr_t)&_stack_top, [1] = (uintptr_t)Reset_Handler,
    [2] = (uintptr_t)Default_Handler, [3] = (uintptr_t)Default_Handler,
    [11] = (uintptr_t)Default_Handler, [14] = (uintptr_t)Default_Handler,
    [15] = (uintptr_t)Default_Handler,
    [16 ... 34] = (uintptr_t)Default_Handler,
    [37 ... 40] = (uintptr_t)Default_Handler
};
void Reset_Handler(void)
{
    uint32_t *s = &_data_load;
    for (uint32_t *d = &_data_start; d < &_data_end;) *d++ = *s++;
    for (uint32_t *d = &_bss_start; d < &_bss_end;) *d++ = 0;
    *(volatile uint32_t *)0xe000ed08 = 0x4000;
    __asm volatile("dsb\n\tisb" ::: "memory");
    (void)main();
    Default_Handler();
}
