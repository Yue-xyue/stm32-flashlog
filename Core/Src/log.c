/*
 * log.c
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

/* log.c — append-only log over SPI NOR flash
 *
 * 斷電恢復的規則（2026/10 改版）：
 *   1. header 自己有 CRC-16（放在原本的 reserved）。驗得過，rec_id 和 length 才拿來用。
 *   2. 掃描遇到不可信的 header，只放棄那個 sector 剩下的部分，從下一個 sector 開頭繼續。
 *      record 不跨 sector，所以 sector 開頭一定是 record 的起點。
 *      尾端只在最新的 sector 裡找，壞掉的資料不再被當成尾端。
 *   3. 跨進新 sector 只有一個入口 enter_sector()：一律先擦，再寫。
 *      讀起來全是 FF 不代表擦完（擦到一半斷電也可能讀成 FF），所以不靠讀來判斷。
 *      log 是空的時候，第一筆寫入前也先擦 S0。
 *      所以：寫進一個 sector 之前，一定是自己擦完的；開機後接著寫的 sector，
 *      裡面第一筆可信的 header 就證明當初擦完了。
 *   4. 執行中寫入或擦除失敗，跟斷電一樣處理：停止寫入，不在執行中猜 flash 上寫了多少；
 *      remount 或重開時，由 log_init 從 flash 重新判斷位置和編號。
 */

#include "log.h"
#include "log_uart.h"
#include <string.h>
#include <stdio.h>
#include "crc32.h"
#include "perf.h"
#include "wear.h"

static uint32_t s_write_ptr;     /* 下一筆要寫的位址；等於 sector 結尾代表這個 sector 已滿 */
static uint32_t s_next_id;       /* 下一筆的 rec_id */
static uint32_t s_oldest_addr;   /* 最舊 sector 的起始位址（繞一圈時的回收目標） */
static uint32_t s_active_addr;   /* 目前寫入中的 sector 起始位址 */
static int      s_have_data;     /* log 裡有沒有任何 record */
static int      s_recovered;     /* 這次開機有沒有放棄寫壞的尾端 */
static int      s_need_erase;    /* 目前的 sector 這次還沒擦過，寫入前要先進 enter_sector（log 是空的時候） */
static int      s_write_failed;  /* 寫入或擦除失敗過：flash 上的狀態不確定，log_init 之前不再寫 */
static uint32_t s_erase_count;   /* 本次開機以來的擦除次數（暫存在 RAM） */

static uint32_t s_last_append_us;
static uint32_t s_last_read_us;
static uint32_t s_last_init_us;

typedef enum { LOG_MODE_CIRCULAR = 0, LOG_MODE_STOP } log_mode_t;
static log_mode_t s_mode = LOG_MODE_CIRCULAR;
static uint32_t   s_last_erase_us;

/* ------------------------------------------------------------------ 位址 */

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

/* ------------------------------------------------------------------ CRC */

/* CRC-16/CCITT-FALSE，只保護 header 本身 */
static uint16_t crc16_update(uint16_t crc, const uint8_t *p, uint32_t len)
{
    while (len--) {
        crc ^= (uint16_t)((uint16_t)(*p++) << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* header 的 CRC：涵蓋 magic、rec_id、length、timestamp（hcrc 自己當 0 算，不含最後的 crc32） */
static uint16_t header_crc(const rec_header_t *h)
{
    rec_header_t tmp = *h;
    tmp.hcrc = 0;
    return crc16_update(0xFFFFu, (const uint8_t *)&tmp, sizeof(tmp) - 4);
}

/* 整筆的 CRC32：header 前 16 bytes（含 hcrc）＋ payload */
static uint32_t record_crc(const rec_header_t *h, const uint8_t *payload)
{
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)h, sizeof(rec_header_t) - 4);
    crc = crc32_update(crc, payload, h->length);
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ flash 輔助 */

/* 照 page 邊界分段寫入（flash_page_program 不能跨頁） */
static log_status_t program_bytes(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len > 0) {
        uint32_t page_left = FLASH_PAGE_SIZE - (addr % FLASH_PAGE_SIZE);
        uint32_t chunk     = (len < page_left) ? len : page_left;
        if (flash_page_program(addr, buf, (uint16_t)chunk) != FL_OK) return LOG_ERR_IO;
        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    return LOG_OK;
}

/* [addr, end) 是不是全部都是 FF */
static int range_is_blank(uint32_t addr, uint32_t end)
{
    uint8_t buf[64];
    while (addr < end) {
        uint32_t n = end - addr;
        if (n > sizeof(buf)) n = sizeof(buf);
        flash_read(addr, buf, n);
        for (uint32_t i = 0; i < n; i++)
            if (buf[i] != 0xFF) return 0;
        addr += n;
    }
    return 1;
}

/* ------------------------------------------------------------------ 讀 header */

typedef enum {
    HDR_END = 0,   /* 整個 header 都是 FF，或剩下的空間放不下 header：這個 sector 到此為止 */
    HDR_PAD,       /* PAD：這個 sector 剩下的不用了 */
    HDR_OK,        /* header 可信：rec_id、length 都能用（payload 另外驗） */
    HDR_BAD,       /* header 不可信：這個 sector 剩下的都不能信 */
} hdr_kind_t;

static hdr_kind_t read_header(uint32_t addr, rec_header_t *h)
{
    uint32_t sec_end = sector_end_of(addr);
    if (sec_end - addr < sizeof(*h)) return HDR_END;

    flash_read(addr, (uint8_t *)h, sizeof(*h));

    const uint8_t *p = (const uint8_t *)h;
    int all_ff = 1;
    for (uint32_t i = 0; i < sizeof(*h); i++)
        if (p[i] != 0xFF) { all_ff = 0; break; }
    if (all_ff) return HDR_END;

    if (h->hcrc != header_crc(h)) return HDR_BAD;            /* header 寫到一半或位元翻轉 */
    if (h->magic == LOG_MAGIC_PAD) return HDR_PAD;
    if (h->magic != LOG_MAGIC) return HDR_BAD;
    if (h->length == 0 || h->length > LOG_MAX_PAYLOAD) return HDR_BAD;
    if (addr + sizeof(*h) + h->length > sec_end) return HDR_BAD;
    return HDR_OK;
}

/* header 已經可信，再驗 payload */
static int payload_ok(uint32_t addr, const rec_header_t *h)
{
    uint8_t payload[LOG_MAX_PAYLOAD];
    flash_read(addr + sizeof(*h), payload, h->length);
    return record_crc(h, payload) == h->crc32;
}

/* ------------------------------------------------------------------ 走訪 */

/* 從最舊的 sector 走到目前的 sector。init、read、dump、stats 共用同一套規則。 */
typedef struct {
    uint32_t sec;      /* 目前 sector 起點 */
    uint32_t addr;     /* 目前位置 */
    uint32_t left;     /* 還要走幾個 sector（含目前這個） */
} log_iter_t;

typedef enum { IT_DONE = 0, IT_RECORD, IT_BAD } it_kind_t;

static uint32_t sectors_in_use(void)
{
    if (!s_have_data) return 0;
    return (sector_index(s_active_addr) - sector_index(s_oldest_addr) + LOG_SECTOR_COUNT)
           % LOG_SECTOR_COUNT + 1;
}

static void iter_begin(log_iter_t *it)
{
    it->sec  = s_oldest_addr;
    it->addr = s_oldest_addr;
    it->left = sectors_in_use();
}

/* IT_RECORD：*addr 是一筆 header 可信的 record（payload 還沒驗）
   IT_BAD   ：*addr 的 header 不可信；這個 sector 剩下的已經跳過
   IT_DONE  ：走完了 */
static it_kind_t iter_next(log_iter_t *it, uint32_t *addr, rec_header_t *h)
{
    while (it->left > 0) {
        /* 最後一筆剛好寫到 sector 結尾時，addr 已經是下一個 sector 的開頭：這個 sector 到此為止 */
        hdr_kind_t k = (it->addr >= it->sec + FLASH_SECTOR_SIZE) ? HDR_END : read_header(it->addr, h);
        if (k == HDR_OK) {
            *addr = it->addr;
            it->addr += sizeof(*h) + h->length;
            return IT_RECORD;
        }

        /* END / PAD / BAD：這個 sector 結束，換下一個 */
        uint32_t here = it->addr;
        it->left--;
        it->sec  = next_sector_addr(it->sec);
        it->addr = it->sec;
        if (k == HDR_BAD) { *addr = here; return IT_BAD; }
    }
    return IT_DONE;
}

/* 走訪全部 record，分別計算有效與損毀的筆數（header 不可信的區段算一筆損毀） */
static void count_records(uint32_t *valid, uint32_t *corrupt)
{
    log_iter_t   it;
    rec_header_t h;
    uint32_t     addr;
    it_kind_t    k;

    *valid = *corrupt = 0;
    iter_begin(&it);
    while ((k = iter_next(&it, &addr, &h)) != IT_DONE) {
        if (k == IT_RECORD && payload_ok(addr, &h)) (*valid)++;
        else                                         (*corrupt)++;
    }
}

static void print_record(uint32_t addr, const rec_header_t *h)
{
    uint8_t payload[LOG_MAX_PAYLOAD + 1];
    int ok = payload_ok(addr, h);

    flash_read(addr + sizeof(*h), payload, h->length);
    payload[h->length] = '\0';

    log_printf("  [%u] @0x%06X len=%u t=%u crc=%s \"%s\"\r\n",
               (unsigned)h->rec_id, (unsigned)addr,
               (unsigned)h->length, (unsigned)h->timestamp,
               ok ? "OK" : "BAD", payload);
}

/* ------------------------------------------------------------------ 換 sector */

/* 在 addr 處寫一筆 PAD record，宣告「本 sector 剩下的不用了」；sec_end 是這個 sector 的結尾 */
static log_status_t write_pad(uint32_t addr, uint32_t sec_end)
{
    if (addr >= sec_end || sec_end - addr < sizeof(rec_header_t)) return LOG_OK;   /* 放不下 header，留 FF */

    rec_header_t h;
    h.magic     = LOG_MAGIC_PAD;
    h.rec_id    = 0;
    h.length    = 0;
    h.timestamp = HAL_GetTick();
    h.crc32     = 0;
    h.hcrc      = header_crc(&h);
    return program_bytes(addr, (const uint8_t *)&h, sizeof(h));
}

/* 跨進一個新 sector 的唯一入口（format 除外）：一律先擦。
   讀起來全是 FF 也擦：擦到一半斷電的 sector 可能讀起來全是 FF，但沒擦完，寫進去的資料之後可能讀錯。
   擦的是最舊的 sector，代表繞了一圈，最舊往前推一格。 */
static log_status_t enter_sector(uint32_t sec)
{
    int wraps = s_have_data && (sec == s_oldest_addr);
    if (wraps && s_mode == LOG_MODE_STOP) return LOG_ERR_FULL;

    uint32_t te = perf_cycles();
    if (flash_sector_erase(sec) != FL_OK) return LOG_ERR_IO;
    wear_record_erase(sector_index(sec));
    s_last_erase_us = perf_us_since(te);
    s_erase_count++;

    if (wraps)        s_oldest_addr = next_sector_addr(s_oldest_addr);   /* 那些 record 沒了 */
    if (!s_have_data) s_oldest_addr = sec;
    s_active_addr = sec;
    s_write_ptr   = sec;
    s_need_erase  = 0;
    return LOG_OK;
}

/* 確保目前的 sector 放得下 total bytes；放不下（含剛好寫滿、開機時判定已滿）就換 sector */
static log_status_t make_room(uint32_t total)
{
    if (s_need_erase) return enter_sector(s_active_addr);   /* log 是空的：S0 先擦再寫 */

    uint32_t sec_end = sector_end_of(s_active_addr);
    if (s_write_ptr + total <= sec_end) return LOG_OK;

    if (write_pad(s_write_ptr, sec_end) != LOG_OK) return LOG_ERR_IO;
    s_write_ptr = sec_end;   /* PAD 寫好，這個 sector 就關了：換 sector 沒成功的話，下一筆也不能再寫進來 */
    return enter_sector(next_sector_addr(s_active_addr));
}

/* 寫入或擦除到一半失敗：flash 上實際寫了多少不知道，RAM 裡的 write pointer 也不可信了。
   跟斷電一樣處理：不在這裡猜，停止寫入；remount 或重開時由 log_init 從 flash 重新判斷。
   （只把 write pointer 移到 sector 結尾不夠：header 可能已經完整寫進去，再用同一個編號會重號。） */
static log_status_t stop_writing(void)
{
    if (!s_write_failed)
        log_printf("[LOG ] flash write/erase failed near 0x%06X, writes stopped until remount\r\n",
                   (unsigned)s_write_ptr);
    s_write_failed = 1;
    return LOG_ERR_IO;
}

/* 寫入前的共同檢查：失敗過就不寫；空間不夠就換 sector（換的時候失敗也一樣停） */
static log_status_t room_for(uint32_t total)
{
    if (s_write_failed) return LOG_ERR_IO;
    log_status_t st = make_room(total);
    return (st == LOG_ERR_IO) ? stop_writing() : st;
}

/* ------------------------------------------------------------------ 開機 */

/* 在最新的 sector 裡找下一筆該寫的位置。
   *tail: 0 = 正常；1 = 尾端寫到一半或停在 PAD（rec_id 已算進去）；2 = 尾端有讀不出 rec_id 的資料 */
static uint32_t find_tail(uint32_t sec, int *tail)
{
    uint32_t end  = sector_end_of(sec);
    uint32_t addr = sec;
    rec_header_t h;

    *tail = 0;
    while (addr < end) {
        hdr_kind_t k = read_header(addr, &h);
        if (k == HDR_OK) {
            if (!payload_ok(addr, &h)) { *tail = 1; return end; }   /* 寫到一半：這個 sector 不再寫 */
            addr += sizeof(h) + h.length;
            continue;
        }
        if (k == HDR_PAD) { *tail = 1; return end; }   /* 換 sector 時斷電：PAD 寫了，下一個還沒寫 */
        if (k == HDR_BAD) { *tail = 2; return end; }   /* header 寫到一半，rec_id 不可信 */
        break;                                          /* HDR_END */
    }

    /* 尾端之後到 sector 結尾都必須是 FF，才能接著寫 */
    if (!range_is_blank(addr, end)) { *tail = 2; return end; }
    return addr;
}

/* 掃描 flash，推回 oldest、active、write pointer、next_id（這些都不存在 flash 上） */
log_status_t log_init(void)
{
    uint32_t     t0 = perf_cycles();
    rec_header_t h;
    uint32_t     valid, corrupt;

    s_next_id      = 1;
    s_recovered    = 0;
    s_have_data    = 0;
    s_need_erase   = 0;
    s_write_failed = 0;   /* 下面會從 flash 重新判斷，之前寫失敗的地方會被當成壞尾端處理 */

    /* 1. 排新舊：只看 header 驗得過的 sector 開頭 */
    uint32_t min_id = 0, max_id = 0;
    uint32_t oldest = LOG_AREA_START, newest = LOG_AREA_START;
    for (uint32_t i = 0; i < LOG_SECTOR_COUNT; i++) {
        uint32_t a = sector_addr(i);
        if (read_header(a, &h) != HDR_OK) continue;      /* 空的、壞掉的：不參與排序 */
        if (!s_have_data || h.rec_id < min_id) { min_id = h.rec_id; oldest = a; }
        if (!s_have_data || h.rec_id > max_id) { max_id = h.rec_id; newest = a; }
        s_have_data = 1;
    }

    if (!s_have_data) {
        /* 沒有任何可信的 record：從 S0 開始，第一筆寫入前先擦 S0。
           S0 可能有寫一半的 header，或擦到一半（讀起來像 FF）。不是全 FF 的話標記 recovered。 */
        s_oldest_addr = s_active_addr = s_write_ptr = LOG_AREA_START;
        s_need_erase  = 1;
        s_recovered   = !range_is_blank(LOG_AREA_START, sector_end_of(LOG_AREA_START));
    } else {
        s_oldest_addr = oldest;
        s_active_addr = newest;

        /* 2. 從最舊掃到最新：壞掉的只跳過那個 sector 剩下的，不當成尾端 */
        log_iter_t it;
        uint32_t   addr;
        it_kind_t  k;
        iter_begin(&it);
        while ((k = iter_next(&it, &addr, &h)) != IT_DONE) {
            if (k == IT_RECORD) {
                if (h.rec_id > max_id) max_id = h.rec_id;
            } else {
                log_printf("[LOG ] bad header @0x%06X, skip rest of sector\r\n", (unsigned)addr);
            }
        }

        /* 3. 尾端只在最新的 sector 裡找 */
        int tail;
        s_write_ptr = find_tail(s_active_addr, &tail);
        s_next_id   = max_id + 1 + (tail == 2 ? 1 : 0);   /* 讀不出 rec_id 的那筆，編號直接跳過 */
        s_recovered = (tail != 0);
    }

    s_last_init_us = perf_us_since(t0);

    count_records(&valid, &corrupt);
    log_printf("[LOG ] init: %u valid, %u corrupt, wp=0x%06X%s, next_id=%u "
               "oldest=S%u active=S%u%s\r\n",
               (unsigned)valid, (unsigned)corrupt, (unsigned)s_write_ptr,
               (s_write_ptr == sector_end_of(s_active_addr)) ? " (full)" : "",
               (unsigned)s_next_id,
               (unsigned)sector_index(s_oldest_addr),
               (unsigned)sector_index(s_active_addr),
               s_recovered ? " (recovered)" : "");
    return LOG_OK;
}

/* ------------------------------------------------------------------ 寫入 */

log_status_t log_append(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > LOG_MAX_PAYLOAD) return LOG_ERR_PARAM;

    uint32_t total = sizeof(rec_header_t) + len;
    log_status_t st = room_for(total);
    if (st != LOG_OK) return st;

    rec_header_t h;
    h.magic     = LOG_MAGIC;
    h.rec_id    = s_next_id;
    h.length    = len;
    h.timestamp = HAL_GetTick();
    h.crc32     = 0;
    h.hcrc      = header_crc(&h);          /* 先算 header 自己的 CRC */
    h.crc32     = record_crc(&h, data);    /* 再算整筆的 CRC（含 hcrc） */

    uint8_t buf[sizeof(rec_header_t) + LOG_MAX_PAYLOAD];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, len);

    /* 逐頁寫入（record 保證不跨 sector，但仍可能跨 page） */
    uint32_t t0 = perf_cycles();
    st = program_bytes(s_write_ptr, buf, total);
    s_last_append_us = perf_us_since(t0);
    if (st != LOG_OK) return stop_writing();

    s_write_ptr += total;
    s_next_id++;
    s_have_data = 1;
    return LOG_OK;
}

/* ------------------------------------------------------------------ 讀取 */

log_status_t log_read(uint32_t rec_id, uint8_t *buf, uint16_t *len)
{
    uint32_t     t0 = perf_cycles();
    log_iter_t   it;
    uint32_t     addr;
    rec_header_t h;
    it_kind_t    k;

    iter_begin(&it);
    while ((k = iter_next(&it, &addr, &h)) != IT_DONE) {
        if (k != IT_RECORD || h.rec_id != rec_id) continue;
        if (!payload_ok(addr, &h)) {
            s_last_read_us = perf_us_since(t0);
            return LOG_ERR_CRC;
        }
        uint16_t n = (h.length < *len) ? h.length : *len;
        flash_read(addr + sizeof(h), buf, n);
        *len = n;
        s_last_read_us = perf_us_since(t0);
        return LOG_OK;
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
    it_kind_t    k;

    if (count == 0) count = 20;

    iter_begin(&it);

    if (start_id == 0) {
        uint32_t valid, corrupt;
        count_records(&valid, &corrupt);
        uint32_t total = valid + corrupt;      /* dump 要顯示全部，含損毀 */
        uint32_t skip  = (total > count) ? (total - count) : 0;
        while (skip-- > 0 && iter_next(&it, &addr, &h) != IT_DONE) { }
    }

    while (count > 0 && (k = iter_next(&it, &addr, &h)) != IT_DONE) {
        if (k == IT_BAD) {
            log_printf("  [?] @0x%06X bad header, rest of sector skipped\r\n", (unsigned)addr);
            count--;
            continue;
        }
        if (start_id != 0 && h.rec_id < start_id) continue;   /* 還沒到 */
        print_record(addr, &h);
        count--;
    }
}

void log_stats(void)
{
    uint32_t valid, corrupt;
    count_records(&valid, &corrupt);

    log_printf("[LOG ] records=%u corrupt=%u sectors=%u/%u wp=0x%06X next_id=%u\r\n",
               (unsigned)valid, (unsigned)corrupt, (unsigned)sectors_in_use(), LOG_SECTOR_COUNT,
               (unsigned)s_write_ptr, (unsigned)s_next_id);
    log_printf("[LOG ] last_append_us=%u last_read_us=%u init_us=%u\r\n",
               (unsigned)s_last_append_us, (unsigned)s_last_read_us,
               (unsigned)s_last_init_us);
    log_printf("[LOG ] oldest=S%u active=S%u erases=%u last_erase_us=%u\r\n",
               (unsigned)sector_index(s_oldest_addr),
               (unsigned)sector_index(s_active_addr),
               (unsigned)s_erase_count, (unsigned)s_last_erase_us);
    if (s_write_failed)
        log_printf("[LOG ] writes stopped after a flash failure: run log remount\r\n");
}

/* 清空 log 區（目前只擦前 16 個 sector = 64KB，夠測試用） */
log_status_t log_format(void)
{
    for (uint32_t a = LOG_AREA_START; a < LOG_AREA_END; a += FLASH_SECTOR_SIZE) {
        if (flash_sector_erase(a) != FL_OK) return stop_writing();   /* 擦到一半：RAM 裡的狀態已經不對 */
    }
    s_write_ptr    = LOG_AREA_START;
    s_oldest_addr  = LOG_AREA_START;
    s_active_addr  = LOG_AREA_START;
    s_next_id      = 1;
    s_have_data    = 0;
    s_recovered    = 0;
    s_erase_count  = 0;
    s_need_erase   = 0;   /* 剛全部擦完 */
    s_write_failed = 0;
    return LOG_OK;
}

/* ------------------------------------------------------------------ 測試用：故障注入 */

/* 準備一個 header（magic、rec_id、length、timestamp、hcrc 都正確） */
static void make_header(rec_header_t *h, const uint8_t *data, uint16_t len)
{
    h->magic     = LOG_MAGIC;
    h->rec_id    = s_next_id;
    h->length    = len;
    h->timestamp = HAL_GetTick();
    h->crc32     = 0;
    h->hcrc      = header_crc(h);
    h->crc32     = record_crc(h, data);
}

/* 故意寫一筆 CRC 錯誤的 record（header 正確，payload 的 CRC 不對；模擬資料損毀） */
log_status_t log_inject_corrupt(void)
{
    const char *msg = "CORRUPTED";
    uint16_t len = 9;

    log_status_t st = room_for(sizeof(rec_header_t) + len);
    if (st != LOG_OK) return st;

    rec_header_t h;
    make_header(&h, (const uint8_t *)msg, len);
    h.crc32 = 0xDEADBEEFu;                 /* 故意填錯 */

    uint8_t buf[sizeof(h) + 16];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), msg, len);

    if (program_bytes(s_write_ptr, buf, sizeof(h) + len) != LOG_OK) return stop_writing();

    log_printf("[LOG ] injected corrupt record @0x%06X\r\n", (unsigned)s_write_ptr);
    return LOG_OK;   /* 刻意不更新 write_ptr：接著用 log remount 模擬斷電重開 */
}

/* 只寫 header + 一半 payload（模擬寫入中斷電） */
log_status_t log_inject_partial(const uint8_t *data, uint16_t len)
{
    if (len < 2 || len > LOG_MAX_PAYLOAD) return LOG_ERR_PARAM;
    log_status_t st = room_for(sizeof(rec_header_t) + len);
    if (st != LOG_OK) return st;

    rec_header_t h;
    make_header(&h, data, len);            /* CRC 是完整資料的 */

    /* 但只寫 header + 一半 payload */
    uint16_t half = len / 2;
    uint8_t buf[sizeof(h) + LOG_MAX_PAYLOAD];
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, half);

    if (program_bytes(s_write_ptr, buf, sizeof(h) + half) != LOG_OK) return stop_writing();

    log_printf("[LOG ] injected partial record @0x%06X (%u of %u bytes)\r\n",
               (unsigned)s_write_ptr, half, len);
    return LOG_OK;
}

/* 只寫 header 的前 8 bytes（magic + rec_id）：模擬 header 寫到一半就斷電 */
log_status_t log_inject_torn_header(void)
{
    const uint8_t dummy[4] = { 't', 'o', 'r', 'n' };
    log_status_t st = room_for(sizeof(rec_header_t) + sizeof(dummy));
    if (st != LOG_OK) return st;

    rec_header_t h;
    make_header(&h, dummy, sizeof(dummy));
    if (program_bytes(s_write_ptr, (const uint8_t *)&h, 8) != LOG_OK) return stop_writing();

    log_printf("[LOG ] injected torn header @0x%06X (8 of %u bytes)\r\n",
               (unsigned)s_write_ptr, (unsigned)sizeof(h));
    return LOG_OK;
}

/* 在 write pointer 寫一筆 PAD 就停：模擬換 sector 時，PAD 寫完、下一個 sector 還沒寫就斷電 */
log_status_t log_inject_pad_cut(void)
{
    if (s_write_failed) return LOG_ERR_IO;
    if (s_need_erase) {                     /* log 是空的：跟一般寫入一樣，S0 先擦 */
        log_status_t st = room_for(sizeof(rec_header_t));
        if (st != LOG_OK) return st;
    }
    uint32_t left = sector_end_of(s_active_addr) - s_write_ptr;
    if (left < sizeof(rec_header_t)) {
        log_printf("[LOG ] no room for PAD in this sector\r\n");
        return LOG_ERR_FULL;
    }
    if (write_pad(s_write_ptr, sector_end_of(s_active_addr)) != LOG_OK) return stop_writing();

    log_printf("[LOG ] injected PAD @0x%06X (power cut before next sector)\r\n",
               (unsigned)s_write_ptr);
    return LOG_OK;
}

/* 連續寫 count 筆、每筆 len bytes：用來測剛好寫滿、繞一圈 */
log_status_t log_fill(uint32_t count, uint16_t len)
{
    if (len == 0 || len > LOG_MAX_PAYLOAD) return LOG_ERR_PARAM;

    uint8_t buf[LOG_MAX_PAYLOAD];
    uint32_t done = 0;
    log_status_t st = LOG_OK;

    while (done < count) {
        char tag[16];
        int n = snprintf(tag, sizeof(tag), "fill%05u", (unsigned)(s_next_id % 100000u));
        memset(buf, '.', len);
        memcpy(buf, tag, (n < (int)len) ? (uint32_t)n : len);
        st = log_append(buf, len);
        if (st != LOG_OK) break;
        done++;
    }

    log_printf("[LOG ] fill: %u of %u records (%u B each), wp=0x%06X next_id=%u active=S%u\r\n",
               (unsigned)done, (unsigned)count, (unsigned)len, (unsigned)s_write_ptr,
               (unsigned)s_next_id, (unsigned)sector_index(s_active_addr));
    return st;
}

/* 測試用：目前看得到的有效、損毀筆數 */
void log_get_counts(uint32_t *valid, uint32_t *corrupt)
{
    count_records(valid, corrupt);
}

uint32_t log_last_append_us(void) { return s_last_append_us; }
uint32_t log_last_read_us(void)   { return s_last_read_us; }
uint32_t log_last_init_us(void)   { return s_last_init_us; }
