/* SPDX-License-Identifier: MIT */
#include "samd20.h"
#include "samd20g18.h"
static m1_runtime runtime;
int main(void)
{
    m1_io io;
    if(!m1_platform_init(&io)) NVIC_SystemReset();
    m1_runtime_init(&runtime,&io,m1_platform_millis());
    for(;;) {
        m1_platform_drain(&runtime);
        m1_runtime_poll(&runtime,m1_platform_millis(),m1_platform_button());
        m1_platform_watchdog();
        __WFI();
    }
}
