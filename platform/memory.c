/* SPDX-License-Identifier: GPL-3.0-or-later */
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
int memcmp(const void *left,const void *right,size_t count)
{
    const unsigned char *a=left,*b=right;
    while(count--){if(*a!=*b)return (int)*a-(int)*b;++a;++b;}
    return 0;
}
