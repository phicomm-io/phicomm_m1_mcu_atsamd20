/* SPDX-License-Identifier: MIT */
#ifndef M1_MEMORY_H
#define M1_MEMORY_H
#include <stddef.h>
void *memset(void *,int,size_t);
void *memcpy(void *,const void *,size_t);
int memcmp(const void *,const void *,size_t);
#endif
