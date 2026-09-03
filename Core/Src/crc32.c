/*
 * crc32.c
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

/* crc32.c — 標準 CRC-32 (IEEE 802.3)，查表法 */

#include "crc32.h"

#define CRC32_POLY_REV  0xEDB88320u

static uint32_t s_table[256];
static int      s_table_ready = 0;

/* 首次使用時建表（256 × 8 次迴圈，約 2048 次，只做一次） */
static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? ((c >> 1) ^ CRC32_POLY_REV) : (c >> 1);
        }
        s_table[i] = c;
    }
    s_table_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!s_table_ready) build_table();

    for (uint32_t i = 0; i < len; i++) {
        crc = s_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}
