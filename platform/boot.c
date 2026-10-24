/* SPDX-License-Identifier: MIT */
#include "samd20.h"
#include "samd20g18.h"
int main(void)
{
    if(!m1_platform_clock()) NVIC_SystemReset();
    m1_flash flash=m1_platform_flash();
    m1_boot_result result=m1_boot_apply(&flash);
    if(result==M1_BOOT_NORMAL || result==M1_BOOT_UPDATED) {
        uint8_t vector[8];
        if(flash.read(flash.context,M1_APPLICATION_ADDRESS,vector,8) && m1_image_vector_valid(vector,M1_MAX_IMAGE))
            m1_platform_jump();
    }
    /* Preserve staging/journal on failure. Watchdog re-enters Boot, allowing
     * a reset interrupted copy to restart. Invalid images require SWD recovery. */
    for(;;) __WFI();
}
