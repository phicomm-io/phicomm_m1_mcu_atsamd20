/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef M1_MUSIC_H
#define M1_MUSIC_H
#include "m1/update.h"
#define M1_MUSIC_ADDRESS 0x0003e000u
#define M1_MUSIC_SLOT 2048u
#define M1_MUSIC_MAX 1024u
typedef struct {
    char text[M1_MUSIC_MAX+1], pending[M1_MUSIC_MAX+1];
    uint32_t generation, crc, duration, upload_crc, upload_at, note_at, until;
    uint16_t length, received, expected, position, notes, bpm;
    uint8_t slot, denominator, octave;
    bool present, uploading, playing, repeat;
} m1_music;
bool m1_rtttl_validate(const char *,size_t,uint32_t *duration);
void m1_music_load(m1_music *,const m1_flash *);
uint8_t m1_music_command(m1_music *,const m1_flash *,const uint8_t args[7],uint8_t data[14],uint32_t now);
/* 0=finished, 1=unchanged, 2=apply Hz (zero is a rest). */
unsigned m1_music_tick(m1_music *,uint32_t now,uint16_t *hz);
void m1_music_stop(m1_music *);
#endif
