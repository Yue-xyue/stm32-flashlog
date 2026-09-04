/* wear.c — 以 append-only 事件記錄各 sector 的擦除次數 */

#include "wear.h"
#include "log_uart.h"

#define WEAR_AREA_START  0x000000u
#define WEAR_MAGIC       0x57450000u    /* "WE" */
#define WEAR_MASK        0xFFFF0000u
#define WEAR_EVENT_SIZE  4
#define WEAR_MAX_EVENTS  (FLASH_SECTOR_SIZE / WEAR_EVENT_SIZE)

static uint32_t s_count[WEAR_MAX_SECTORS];
static uint32_t s_write_off;             /* 下一筆事件的偏移量 */

/* 開機掃描：把所有事件重播一次，重建計數 */
void wear_init(void)
{
    uint32_t buf[64];                    /* 一次讀 256 bytes */

    for (uint32_t i = 0; i < WEAR_MAX_SECTORS; i++) s_count[i] = 0;
    s_write_off = 0;

    for (uint32_t off = 0; off < FLASH_SECTOR_SIZE; off += sizeof(buf)) {
        flash_read(WEAR_AREA_START + off, (uint8_t *)buf, sizeof(buf));

        for (uint32_t k = 0; k < 64; k++) {
            uint32_t e = buf[k];

            if (e == 0xFFFFFFFFu) {                  /* 空白 → 到此為止 */
                s_write_off = off + k * WEAR_EVENT_SIZE;
                return;
            }
            if ((e & WEAR_MASK) != WEAR_MAGIC) continue;   /* 不認得，跳過 */

            uint32_t sec = e & 0xFFFFu;
            if (sec < WEAR_MAX_SECTORS) s_count[sec]++;
        }
    }
    s_write_off = FLASH_SECTOR_SIZE;      /* 滿了 */
}

/* 把目前的統計壓縮成「每 sector 一筆總結」重寫（metadata 滿時呼叫） */
static void wear_compact(void)
{
    uint32_t saved[WEAR_MAX_SECTORS];
    for (uint32_t i = 0; i < WEAR_MAX_SECTORS; i++) saved[i] = s_count[i];

    if (flash_sector_erase(WEAR_AREA_START) != FL_OK) return;
    s_write_off = 0;

    /* 重寫：每個 sector 寫 count 筆事件會爆掉，所以改寫「總結事件」。
       這裡採最簡單的做法：只保留相對差異，全部減去最小值。 */
    uint32_t base = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < WEAR_MAX_SECTORS; i++)
        if (saved[i] < base) base = saved[i];

    for (uint32_t i = 0; i < WEAR_MAX_SECTORS; i++) {
        uint32_t extra = saved[i] - base;
        for (uint32_t k = 0; k < extra && s_write_off < FLASH_SECTOR_SIZE; k++) {
            uint32_t e = WEAR_MAGIC | i;
            flash_page_program(WEAR_AREA_START + s_write_off,
                               (const uint8_t *)&e, WEAR_EVENT_SIZE);
            s_write_off += WEAR_EVENT_SIZE;
        }
        s_count[i] = base + extra;
    }

    log_printf("[WEAR] compacted, base=%u\r\n", (unsigned)base);
}

void wear_record_erase(uint32_t sector)
{
    if (sector >= WEAR_MAX_SECTORS) return;

    if (s_write_off + WEAR_EVENT_SIZE > FLASH_SECTOR_SIZE) wear_compact();

    uint32_t e = WEAR_MAGIC | sector;
    if (flash_page_program(WEAR_AREA_START + s_write_off,
                           (const uint8_t *)&e, WEAR_EVENT_SIZE) != FL_OK) return;

    s_write_off += WEAR_EVENT_SIZE;
    s_count[sector]++;
}

void wear_get(wear_stats_t *out)
{
    out->total = 0;
    out->min   = 0xFFFFFFFFu;
    out->max   = 0;

    for (uint32_t i = 0; i < WEAR_MAX_SECTORS; i++) {
        uint32_t c = s_count[i];
        out->count[i] = c;
        out->total   += c;
        if (c < out->min) out->min = c;
        if (c > out->max) out->max = c;
    }
    out->events_used = s_write_off / WEAR_EVENT_SIZE;
}

void wear_reset(void)
{
    flash_sector_erase(WEAR_AREA_START);
    for (uint32_t i = 0; i < WEAR_MAX_SECTORS; i++) s_count[i] = 0;
    s_write_off = 0;
}
