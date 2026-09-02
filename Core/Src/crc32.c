/*
 * crc32.c
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

/* crc32.c — 標準 CRC-32 (IEEE 802.3)，逐位元計算版本 */

#include "crc32.h"

#define CRC32_POLY_REV  0xEDB88320u   /* 0x04C11DB7 的位元反轉 */

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ CRC32_POLY_REV) : (crc >> 1);
        }
    }
    return crc;
}

uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}
