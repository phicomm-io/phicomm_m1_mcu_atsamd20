/* SPDX-License-Identifier: MIT */
#include <stddef.h>
void *memset(void *destination, int value, size_t count)
{
    volatile unsigned char *p = destination;
    while (count--) *p++ = (unsigned char)value;
    return destination;
}
void *memcpy(void *destination, const void *source, size_t count)
{
    volatile unsigned char *d = destination;
    const volatile unsigned char *s = source;
    while (count--) *d++ = *s++;
    return destination;
}
