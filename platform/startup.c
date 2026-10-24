/* SPDX-License-Identifier: MIT */
#include <stdint.h>
extern uint32_t _stack_top, _data_start, _data_end, _data_load, _bss_start, _bss_end;
extern int main(void);
void Reset_Handler(void);
void Default_Handler(void) { for (;;) __asm volatile("wfi"); }
#define WEAK(name) void name(void) __attribute__((weak,alias("Default_Handler")))
WEAK(SysTick_Handler); WEAK(EIC_Handler); WEAK(SERCOM1_Handler); WEAK(SERCOM3_Handler); WEAK(SERCOM4_Handler);
__attribute__((section(".vectors"), used))
const uintptr_t vectors[41] = {
    [0]=(uintptr_t)&_stack_top, [1]=(uintptr_t)Reset_Handler,
    [2]=(uintptr_t)Default_Handler, [3]=(uintptr_t)Default_Handler,
    [11]=(uintptr_t)Default_Handler, [14]=(uintptr_t)Default_Handler,
    [15]=(uintptr_t)SysTick_Handler,
    [16 ... 19]=(uintptr_t)Default_Handler, [20]=(uintptr_t)EIC_Handler,
    [21 ... 23]=(uintptr_t)Default_Handler, [24]=(uintptr_t)SERCOM1_Handler,
    [25]=(uintptr_t)Default_Handler, [26]=(uintptr_t)SERCOM3_Handler, [27]=(uintptr_t)SERCOM4_Handler,
    [28 ... 34]=(uintptr_t)Default_Handler, [37 ... 40]=(uintptr_t)Default_Handler
};
void Reset_Handler(void)
{
    __asm volatile("cpsid i" ::: "memory");
    *(volatile uint32_t *)0xe000e010=0; /* stop inherited SysTick */
    *(volatile uint32_t *)0xe000e180=0xffffffff;
    *(volatile uint32_t *)0xe000e280=0xffffffff;
    *(volatile uint32_t *)0xe000ed04=(1u<<25)|(1u<<27);
    uint32_t *s=&_data_load;
    for(uint32_t *d=&_data_start;d<&_data_end;) *d++=*s++;
    for(uint32_t *d=&_bss_start;d<&_bss_end;) *d++=0;
    *(volatile uint32_t *)0xe000ed08=(uintptr_t)vectors;
    __asm volatile("dsb\n\tisb\n\tcpsie i" ::: "memory");
    (void)main(); Default_Handler();
}
