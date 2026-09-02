/*
 * log.c
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

/* log.c — append-only log over SPI NOR flash */

#include "log.h"
#include "log_uart.h"
#include <string.h>
#include "crc32.h"

static uint32_t s_write_ptr;    /* 下一筆要寫的位址 */
static uint32_t s_next_id;      /* 下一筆的 rec_id */
static uint32_t s_count;        /* 目前總筆數 */
static uint32_t s_bad_addr;

/* CRC 涵蓋 header(不含 crc32 欄位本身)+ payload */
static uint32_t record_crc(const rec_header_t *h, const uint8_t *payload)
{
    uint32_t crc = 0xFFFFFFFFu;
    /* header 前 16 bytes：magic..timestamp，剛好排除最後的 crc32 */
    crc = crc32_update(crc, (const uint8_t *)h, sizeof(rec_header_t) - 4);
    crc = crc32_update(crc, payload, h->length);
    return crc ^ 0xFFFFFFFFu;
}

/* 讀出一筆 record 並驗證，回傳是否有效 */
static int record_verify(uint32_t addr, rec_header_t *h)
{
    uint8_t payload[LOG_MAX_PAYLOAD];

    flash_read(addr, (uint8_t *)h, sizeof(*h));

    if (h->magic != LOG_MAGIC)          return 0;
    if (h->length > LOG_MAX_PAYLOAD)    return 0;
    if (addr + sizeof(*h) + h->length >= LOG_AREA_END) return 0;

    flash_read(addr + sizeof(*h), payload, h->length);

    uint32_t want = h->crc32;
    rec_header_t tmp = *h;
    tmp.crc32 = 0;
    return (record_crc(&tmp, payload) == want);
}

/* 掃描 flash，找出 log 尾端 */
log_status_t log_init(void)
{
    uint32_t     addr = LOG_AREA_START;
    rec_header_t h;

    s_next_id  = 1;
    s_count    = 0;
    s_bad_addr = 0;                 /* 新增：記錄損毀位置 */

    while (addr + sizeof(h) < LOG_AREA_END) {
        flash_read(addr, (uint8_t *)&h, sizeof(h));

        if (h.magic == 0xFFFFFFFFu) break;          /* 空白區 = log 尾端 */

        if (!record_verify(addr, &h)) {
            /* 情況 A 或 B：損毀或寫到一半 */
            s_bad_addr = addr;
            log_printf("[LOG ] corrupt record @0x%06X, truncating here\r\n",
                       (unsigned)addr);
            break;
        }

        s_count++;
        s_next_id = h.rec_id + 1;
        addr += sizeof(h) + h.length;
    }

    s_write_ptr = addr;
    log_printf("[LOG ] init: %u records, wp=0x%06X, next_id=%u%s\r\n",
               (unsigned)s_count, (unsigned)s_write_ptr, (unsigned)s_next_id,
               s_bad_addr ? " (recovered)" : "");
    return LOG_OK;
}

log_status_t log_append(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > LOG_MAX_PAYLOAD) return LOG_ERR_PARAM;

    uint32_t total = sizeof(rec_header_t) + len;
    if (s_write_ptr + total >= LOG_AREA_END) return LOG_ERR_FULL;

    rec_header_t h;
    h.magic     = LOG_MAGIC;
    h.rec_id    = s_next_id;
    h.length    = len;
    h.reserved  = 0;
    h.timestamp = HAL_GetTick();
    h.crc32 	= 0;                          /* 先歸零，避免殘值影響計算 */
    h.crc32 	= record_crc(&h, data);       /* 再算真正的值 */

    /* 組成一塊連續 buffer，減少寫入次數 */
    uint8_t buf[sizeof(rec_header_t) + LOG_MAX_PAYLOAD];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, len);

    /* 逐頁寫入，處理跨頁 */
    uint32_t addr = s_write_ptr;
    uint32_t off  = 0;
    while (off < total) {
        uint32_t page_left = FLASH_PAGE_SIZE - (addr % FLASH_PAGE_SIZE);
        uint32_t chunk     = (total - off < page_left) ? (total - off) : page_left;

        if (flash_page_program(addr, buf + off, (uint16_t)chunk) != FL_OK)
            return LOG_ERR_IO;

        addr += chunk;
        off  += chunk;
    }

    s_write_ptr += total;
    s_next_id++;
    s_count++;
    return LOG_OK;
}

log_status_t log_read(uint32_t rec_id, uint8_t *buf, uint16_t *len)
{
    uint32_t     addr = LOG_AREA_START;
    rec_header_t h;

    while (addr < s_write_ptr) {
        flash_read(addr, (uint8_t *)&h, sizeof(h));
        if (h.magic != LOG_MAGIC) break;

        if (h.rec_id == rec_id) {
            uint16_t n = (h.length < *len) ? h.length : *len;
            flash_read(addr + sizeof(h), buf, n);
            *len = n;
            return LOG_OK;
        }
        addr += sizeof(h) + h.length;
    }
    return LOG_ERR_NOTFOUND;
}

void log_dump(void)
{
    uint32_t     addr = LOG_AREA_START;
    rec_header_t h;
    uint8_t      payload[LOG_MAX_PAYLOAD + 1];

    if (s_count == 0) { log_printf("[LOG ] (empty)\r\n"); return; }

    while (addr < s_write_ptr) {
        flash_read(addr, (uint8_t *)&h, sizeof(h));
        if (h.magic != LOG_MAGIC) break;

        int ok = record_verify(addr, &h);
        uint16_t n = (h.length > LOG_MAX_PAYLOAD) ? LOG_MAX_PAYLOAD : h.length;
        flash_read(addr + sizeof(h), payload, n);
        payload[n] = '\0';

        log_printf("  [%u] @0x%06X len=%u t=%u crc=%s \"%s\"\r\n",
                   (unsigned)h.rec_id, (unsigned)addr,
                   (unsigned)h.length, (unsigned)h.timestamp,
                   ok ? "OK" : "BAD", payload);

        addr += sizeof(h) + h.length;
    }
}

void log_stats(void)
{
    uint32_t used = s_write_ptr - LOG_AREA_START;
    log_printf("[LOG ] records=%u used=%u B wp=0x%06X next_id=%u\r\n",
               (unsigned)s_count, (unsigned)used,
               (unsigned)s_write_ptr, (unsigned)s_next_id);
}

/* 清空 log 區（目前只擦前 16 個 sector = 64KB，夠測試用） */
log_status_t log_format(void)
{
    for (uint32_t a = LOG_AREA_START; a < LOG_AREA_START + 16 * FLASH_SECTOR_SIZE;
         a += FLASH_SECTOR_SIZE) {
        if (flash_sector_erase(a) != FL_OK) return LOG_ERR_IO;
    }
    s_write_ptr = LOG_AREA_START;
    s_next_id   = 1;
    s_count     = 0;
    return LOG_OK;
}

/* 故意寫一筆 CRC 錯誤的 record（模擬資料損毀） */
log_status_t log_inject_corrupt(void)
{
    const char *msg = "CORRUPTED";
    uint16_t len = 9;

    rec_header_t h;
    h.magic     = LOG_MAGIC;
    h.rec_id    = s_next_id;
    h.length    = len;
    h.reserved  = 0;
    h.timestamp = HAL_GetTick();
    h.crc32     = 0xDEADBEEFu;        /* 故意填錯 */

    uint8_t buf[sizeof(h) + 16];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), msg, len);

    if (flash_page_program(s_write_ptr, buf, sizeof(h) + len) != FL_OK)
        return LOG_ERR_IO;

    log_printf("[LOG ] injected corrupt record @0x%06X\r\n", (unsigned)s_write_ptr);
    return LOG_OK;   /* 刻意不更新 write_ptr */
}

/* 只寫 header + 一半 payload（模擬寫入中斷電） */
log_status_t log_inject_partial(const uint8_t *data, uint16_t len)
{
    if (len < 2) return LOG_ERR_PARAM;

    rec_header_t h;
    h.magic     = LOG_MAGIC;
    h.rec_id    = s_next_id;
    h.length    = len;              /* header 說有 len bytes */
    h.reserved  = 0;
    h.timestamp = HAL_GetTick();
    h.crc32     = 0;
    h.crc32     = record_crc(&h, data);   /* CRC 是完整資料的 */

    /* 但只寫 header + 一半 payload */
    uint16_t half = len / 2;
    uint8_t buf[sizeof(h) + LOG_MAX_PAYLOAD];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, half);

    if (flash_page_program(s_write_ptr, buf, sizeof(h) + half) != FL_OK)
        return LOG_ERR_IO;

    log_printf("[LOG ] injected partial record @0x%06X (%u of %u bytes)\r\n",
               (unsigned)s_write_ptr, half, len);
    return LOG_OK;
}
