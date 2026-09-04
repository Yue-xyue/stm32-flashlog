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
#include "perf.h"
#include "wear.h"

static uint32_t s_write_ptr;    /* 下一筆要寫的位址 */
static uint32_t s_next_id;      /* 下一筆的 rec_id */
static uint32_t s_bad_addr;
static uint32_t s_oldest_addr;   /* 最舊 sector 的起始位址（下次回收目標） */
static uint32_t s_active_addr;   /* 目前寫入中的 sector 起始位址 */
static uint32_t s_erase_count;   /* 本次開機以來的擦除次數（暫存在 RAM） */

static uint32_t s_last_append_us;
static uint32_t s_last_read_us;
static uint32_t s_last_init_us;

typedef enum { LOG_MODE_CIRCULAR = 0, LOG_MODE_STOP } log_mode_t;
static log_mode_t s_mode = LOG_MODE_CIRCULAR;
static uint32_t   s_last_erase_us;

/* 環形迭代器狀態 */
typedef struct {
    uint32_t addr;        /* 目前掃描位置 */
    uint32_t sec_addr;    /* 目前 sector 起點 */
    uint32_t sec_done;    /* 已走過幾個 sector */
    int      finished;
} log_iter_t;

/* 本 sector 的結束位址（＝下一個 sector 的起點） */
static uint32_t sector_end_of(uint32_t addr)
{
    return (addr / FLASH_SECTOR_SIZE + 1) * FLASH_SECTOR_SIZE;
}

/* sector 索引 <-> 位址 */
static uint32_t sector_addr(uint32_t idx)
{
    return LOG_AREA_START + idx * FLASH_SECTOR_SIZE;
}

static uint32_t sector_index(uint32_t addr)
{
    return (addr - LOG_AREA_START) / FLASH_SECTOR_SIZE;
}

/* 環形前進：到底了就繞回第 0 個 */
static uint32_t next_sector_addr(uint32_t addr)
{
    uint32_t idx = (sector_index(addr) + 1) % LOG_SECTOR_COUNT;
    return sector_addr(idx);
}

/* CRC 涵蓋 header(不含 crc32 欄位本身)+ payload */
static uint32_t record_crc(const rec_header_t *h, const uint8_t *payload)
{
    uint32_t crc = 0xFFFFFFFFu;
    /* header 前 16 bytes：magic..timestamp，剛好排除最後的 crc32 */
    crc = crc32_update(crc, (const uint8_t *)h, sizeof(rec_header_t) - 4);
    crc = crc32_update(crc, payload, h->length);
    return crc ^ 0xFFFFFFFFu;
}

/* 從 addr 開始，找出下一筆有效 record 的位址。
   回傳 0 表示沒有更多了。 */
static uint32_t next_record(uint32_t addr, uint32_t limit, rec_header_t *h)
{
    while (addr < limit) {
        uint32_t sec_end = sector_end_of(addr);

        if (sec_end - addr < sizeof(*h)) { addr = sec_end; continue; }

        flash_read(addr, (uint8_t *)h, sizeof(*h));

        if (h->magic == 0xFFFFFFFFu) return 0;              /* 結束 */
        if (h->magic == LOG_MAGIC_PAD) { addr = sec_end; continue; }
        if (h->magic != LOG_MAGIC) return 0;                /* 損毀 */

        return addr;                                         /* 找到了 */
    }
    return 0;
}

/* 讀出一筆 record 並驗證，回傳是否有效 */
static int record_verify(uint32_t addr, rec_header_t *h)
{
    uint8_t payload[LOG_MAX_PAYLOAD];

    flash_read(addr, (uint8_t *)h, sizeof(*h));

    if (h->magic != LOG_MAGIC)          return 0;
    if (h->length > LOG_MAX_PAYLOAD)    return 0;
    if (addr + sizeof(*h) + h->length > sector_end_of(addr)) return 0;

    flash_read(addr + sizeof(*h), payload, h->length);

    uint32_t want = h->crc32;
    rec_header_t tmp = *h;
    tmp.crc32 = 0;
    return (record_crc(&tmp, payload) == want);
}

/* 讀某個 sector 的第一筆 record 的 rec_id。
   回傳 0 表示該 sector 是空的或無效。 */
static uint32_t sector_first_id(uint32_t sec_addr)
{
    rec_header_t h;
    flash_read(sec_addr, (uint8_t *)&h, sizeof(h));

    if (h.magic != LOG_MAGIC) return 0;   /* FF、PAD、損毀都算空 */
    return h.rec_id;
}

/* 巡檢所有 sector，找出最舊與最新（active）。
   全空時兩者都指向 sector 0。 */
static void survey_sectors(void)
{
    uint32_t min_id = 0xFFFFFFFFu, max_id = 0;
    uint32_t oldest = LOG_AREA_START, active = LOG_AREA_START;
    int      found  = 0;

    for (uint32_t i = 0; i < LOG_SECTOR_COUNT; i++) {
        uint32_t a  = sector_addr(i);
        uint32_t id = sector_first_id(a);
        if (id == 0) continue;            /* 空 sector */

        found = 1;
        if (id < min_id) { min_id = id; oldest = a; }
        if (id > max_id) { max_id = id; active = a; }
    }

    if (!found) {                          /* 完全空的 log */
        s_oldest_addr = LOG_AREA_START;
        s_active_addr = LOG_AREA_START;
    } else {
        s_oldest_addr = oldest;
        s_active_addr = active;
    }
}

static void iter_begin(log_iter_t *it)
{
    it->addr     = s_oldest_addr;
    it->sec_addr = s_oldest_addr;
    it->sec_done = 0;
    it->finished = 0;
}

/* 取下一筆 record，回傳位址；0 表示走完了 */
static uint32_t iter_next(log_iter_t *it, rec_header_t *h)
{
    while (!it->finished && it->sec_done < LOG_SECTOR_COUNT) {
        uint32_t sec_end = it->sec_addr + FLASH_SECTOR_SIZE;
        uint32_t found   = next_record(it->addr, sec_end, h);

        if (found != 0) {
            it->addr = found + sizeof(*h) + h->length;
            return found;
        }

        /* 本 sector 走完 */
        if (it->sec_addr == s_active_addr) { it->finished = 1; break; }

        it->sec_addr = next_sector_addr(it->sec_addr);
        it->addr     = it->sec_addr;
        it->sec_done++;
    }
    return 0;
}

/* 走訪全部 record 並計數。O(n)，只在需要時呼叫 */
static uint32_t count_records(void)
{
    log_iter_t   it;
    rec_header_t h;
    uint32_t     n = 0;

    iter_begin(&it);
    while (iter_next(&it, &h) != 0) n++;
    return n;
}

static void print_record(uint32_t addr, const rec_header_t *h)
{
    uint8_t payload[LOG_MAX_PAYLOAD + 1];
    rec_header_t tmp = *h;
    int ok = record_verify(addr, &tmp);
    uint16_t n = (h->length > LOG_MAX_PAYLOAD) ? LOG_MAX_PAYLOAD : h->length;

    flash_read(addr + sizeof(*h), payload, n);
    payload[n] = '\0';

    log_printf("  [%u] @0x%06X len=%u t=%u crc=%s \"%s\"\r\n",
               (unsigned)h->rec_id, (unsigned)addr,
               (unsigned)h->length, (unsigned)h->timestamp,
               ok ? "OK" : "BAD", payload);
}

/* 在 addr 處寫一筆 PAD record，宣告「本 sector 剩下的不用了」 */
static log_status_t write_pad(uint32_t addr)
{
    uint32_t end  = sector_end_of(addr);
    uint32_t left = end - addr;

    if (left < sizeof(rec_header_t)) return LOG_OK;   /* 放不下 header，留 FF */

    rec_header_t h;
    h.magic     = LOG_MAGIC_PAD;
    h.rec_id    = 0;
    h.length    = 0;
    h.reserved  = 0;
    h.timestamp = HAL_GetTick();
    h.crc32     = 0;

    if (flash_page_program(addr, (const uint8_t *)&h, sizeof(h)) != FL_OK)
        return LOG_ERR_IO;
    return LOG_OK;
}

/* 掃描 flash，找出 log 尾端 */
log_status_t log_init(void)
{
    uint32_t     t0 = perf_cycles();
    log_iter_t   it;
    rec_header_t h;
    uint32_t     addr;

    s_next_id  = 1;
    s_bad_addr = 0;

    survey_sectors();

    uint32_t last_end = s_oldest_addr;
    iter_begin(&it);

    while ((addr = iter_next(&it, &h)) != 0) {
        if (!record_verify(addr, &h)) {
            s_bad_addr = addr;
            last_end   = addr;
            log_printf("[LOG ] corrupt record @0x%06X\r\n", (unsigned)addr);
            break;
        }
        s_next_id = h.rec_id + 1;
        last_end  = addr + sizeof(h) + h.length;
    }

    s_write_ptr    = last_end;
    s_last_init_us = perf_us_since(t0);

    log_printf("[LOG ] init: %u records, wp=0x%06X, next_id=%u "
               "oldest=S%u active=S%u%s\r\n",
			   (unsigned)count_records(), (unsigned)s_write_ptr, (unsigned)s_next_id,
               (unsigned)sector_index(s_oldest_addr),
               (unsigned)sector_index(s_active_addr),
               s_bad_addr ? " (recovered)" : "");
    return LOG_OK;
}

log_status_t log_append(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > LOG_MAX_PAYLOAD) return LOG_ERR_PARAM;

    uint32_t total = sizeof(rec_header_t) + len;

    /* ---- 本 sector 放不下 → 填 PAD、移到下一個 sector ---- */
    if (s_write_ptr + total > sector_end_of(s_write_ptr)) {
        if (write_pad(s_write_ptr) != LOG_OK) return LOG_ERR_IO;

        uint32_t next_sec = next_sector_addr(s_write_ptr);

        /* ---- 下一個就是最舊的 → 繞了一圈，必須回收 ---- */
        if (next_sec == s_oldest_addr) {
            if (s_mode == LOG_MODE_STOP) return LOG_ERR_FULL;

            uint32_t te = perf_cycles();
            if (flash_sector_erase(next_sec) != FL_OK) return LOG_ERR_IO;
            wear_record_erase(sector_index(next_sec));
            s_last_erase_us = perf_us_since(te);
            s_erase_count++;

            /* 最舊往前推一格（那些 record 沒了） */
            s_oldest_addr = next_sector_addr(s_oldest_addr);
        }

        s_write_ptr   = next_sec;
        s_active_addr = next_sec;
    }

    rec_header_t h;
    h.magic     = LOG_MAGIC;
    h.rec_id    = s_next_id;
    h.length    = len;
    h.reserved  = 0;
    h.timestamp = HAL_GetTick();
    h.crc32     = 0;
    h.crc32     = record_crc(&h, data);

    uint8_t buf[sizeof(rec_header_t) + LOG_MAX_PAYLOAD];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, len);

    /* 逐頁寫入（record 保證不跨 sector，但仍可能跨 page） */
    uint32_t t0   = perf_cycles();
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
    s_last_append_us = perf_us_since(t0);

    s_write_ptr += total;
    s_next_id++;
    return LOG_OK;
}

log_status_t log_read(uint32_t rec_id, uint8_t *buf, uint16_t *len)
{
	uint32_t     t0   = perf_cycles();
	log_iter_t it;
    uint32_t     addr;
    rec_header_t h;

    iter_begin(&it);

    while ((addr = iter_next(&it, &h)) != 0) {
        if (h.rec_id == rec_id) {
            uint16_t n = (h.length < *len) ? h.length : *len;
            flash_read(addr + sizeof(h), buf, n);
            *len = n;
            s_last_read_us = perf_us_since(t0);
            return LOG_OK;
        }
    }

    s_last_read_us = perf_us_since(t0);
    return LOG_ERR_NOTFOUND;
}

/* 印出 record。start_id = 0 表示「最後 count 筆」 */
void log_dump(uint32_t start_id, uint32_t count)
{
    log_iter_t   it;
    rec_header_t h;
    uint32_t     addr;

    if (count == 0) count = 20;

    iter_begin(&it);

    if (start_id == 0) {
        uint32_t total = count_records();
        uint32_t skip  = (total > count) ? (total - count) : 0;
        iter_begin(&it);
        while (skip-- > 0 && iter_next(&it, &h) != 0) { }
    }

    while (count > 0 && (addr = iter_next(&it, &h)) != 0) {
        if (start_id != 0 && h.rec_id < start_id) continue;   /* 還沒到 */
        print_record(addr, &h);
        count--;
    }
}

void log_stats(void)
{
	uint32_t sectors_used = (sector_index(s_active_addr) - sector_index(s_oldest_addr)
	                         + LOG_SECTOR_COUNT) % LOG_SECTOR_COUNT + 1;
	log_printf("[LOG ] records=%u sectors=%u/%u wp=0x%06X next_id=%u\r\n",
			   (unsigned)count_records(), (unsigned)sectors_used, LOG_SECTOR_COUNT,
	           (unsigned)s_write_ptr, (unsigned)s_next_id);
    log_printf("[LOG ] last_append_us=%u last_read_us=%u init_us=%u\r\n",
               (unsigned)s_last_append_us, (unsigned)s_last_read_us,
               (unsigned)s_last_init_us);
    log_printf("[LOG ] oldest=S%u active=S%u erases=%u last_erase_us=%u\r\n",
               (unsigned)sector_index(s_oldest_addr),
               (unsigned)sector_index(s_active_addr),
               (unsigned)s_erase_count, (unsigned)s_last_erase_us);
}

/* 清空 log 區（目前只擦前 16 個 sector = 64KB，夠測試用） */
log_status_t log_format(void)
{
	for (uint32_t a = LOG_AREA_START; a < LOG_AREA_END; a += FLASH_SECTOR_SIZE) {
	    if (flash_sector_erase(a) != FL_OK) return LOG_ERR_IO;
	}
	s_write_ptr   = LOG_AREA_START;
    s_oldest_addr = LOG_AREA_START;
    s_active_addr = LOG_AREA_START;
    s_next_id     = 1;
    s_erase_count = 0;
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

uint32_t log_last_append_us(void) { return s_last_append_us; }
uint32_t log_last_read_us(void)   { return s_last_read_us; }
uint32_t log_last_init_us(void)   { return s_last_init_us; }
