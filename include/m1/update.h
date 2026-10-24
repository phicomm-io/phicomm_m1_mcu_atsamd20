/* SPDX-License-Identifier: MIT */
#ifndef M1_UPDATE_H
#define M1_UPDATE_H
#include "m1/core.h"
#include "m1/board.h"
#define M1_FLASH_ROW 256u
#define M1_FLASH_PAGE 64u
#define M1_MAX_IMAGE 65535u
typedef struct {
    void *context;
    bool (*read)(void *, uint32_t address, uint8_t *data, size_t count);
    bool (*erase)(void *, uint32_t row);
    bool (*write)(void *, uint32_t page, const uint8_t data[64]);
} m1_flash;
typedef struct {
    uint32_t length, last_activity;
    uint16_t next_block;
    uint8_t full_blocks, tail;
    bool active, complete;
} m1_update;
typedef enum { M1_BOOT_NORMAL, M1_BOOT_UPDATED, M1_BOOT_INVALID, M1_BOOT_IO_ERROR } m1_boot_result;
uint32_t m1_crc32(const uint8_t *data, size_t count, uint32_t initial);
bool m1_image_vector_valid(const uint8_t vector[8], uint32_t length);
bool m1_update_begin(m1_update *, uint8_t full, uint8_t tail, uint32_t now);
size_t m1_update_frame_size(const m1_update *, uint16_t block);
void m1_update_block(m1_update *, const m1_flash *, const uint8_t *, size_t, uint32_t, uint8_t reply[20]);
bool m1_update_commit(m1_update *, const m1_flash *);
void m1_update_abort(m1_update *);
m1_boot_result m1_boot_apply(const m1_flash *);
#endif
