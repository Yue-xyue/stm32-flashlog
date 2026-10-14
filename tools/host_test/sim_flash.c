/* sim_flash.c — 在 PC 上模擬 SPI NOR flash，實作 flash.h 的 API
 *
 *   寫入只能把 1 變 0：新值 = 舊值 AND 資料
 *   擦除以 4 KB sector 為單位，全部變回 FF
 *   可以設定「再過 N 次操作就斷電」：那次寫入或擦除只做一部分，然後 longjmp 回測試程式，
 *   測試程式接著呼叫 wear_init() + log_init()，等於斷電後重開機
 *   擦除沒做完的 sector 會被標記「沒擦完」，之後在完整擦除前寫進去就算錯；
 *   斷電在擦除中途時，可以讓它讀起來全是 FF（看起來像擦好了）
 *   也可以設定「再過 N 次操作，flash 就回傳失敗」：只做一部分，回傳 FL_ERR_TIMEOUT，不斷電
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash.h"
#include "sim.h"

uint8_t  sim_mem[SIM_SIZE];
long     sim_ops;
long     sim_erases;
long     sim_cut_countdown;
int      sim_cut_kind;
int      sim_torn_mode;
int      sim_cut_happened;
long     sim_outside_writes;
jmp_buf  sim_cut_jmp;

int      sim_weak_mode;
uint8_t  sim_weak[SIM_SECTORS];
long     sim_weak_cuts;
long     sim_weak_writes;

long     sim_fail_countdown;
int      sim_fail_kind;
int      sim_fail_happened;
long     sim_fails;

static uint64_t s_rng = 88172645463325252ull;

uint32_t sim_rand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

void sim_reset(uint32_t seed)
{
    memset(sim_mem, 0xFF, sizeof(sim_mem));
    sim_ops = sim_erases = 0;
    sim_cut_countdown = 0;
    sim_cut_kind = CUT_ANY;
    sim_torn_mode = TORN_RANDOM;
    sim_cut_happened = 0;
    sim_outside_writes = 0;
    sim_weak_mode = WEAK_NEVER;
    memset(sim_weak, 0, sizeof(sim_weak));
    sim_weak_cuts = sim_weak_writes = 0;
    sim_fail_countdown = 0;
    sim_fail_kind = CUT_ANY;
    sim_fail_happened = 0;
    sim_fails = 0;
    s_rng = 88172645463325252ull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull);
    if (s_rng == 0) s_rng = 1;
    for (int i = 0; i < 8; i++) sim_rand();
}

static int in_log_area(uint32_t addr)
{
    return addr >= SIM_LOG_START && addr < SIM_ALLOWED;
}

static int should_cut(int is_erase)
{
    if (sim_cut_countdown <= 0) return 0;
    if (sim_cut_kind == CUT_ERASE && !is_erase) return 0;
    if (sim_cut_kind == CUT_PROGRAM && is_erase) return 0;
    return --sim_cut_countdown == 0;
}

static int should_fail(uint32_t addr, int is_erase)
{
    if (sim_fail_countdown <= 0 || !in_log_area(addr)) return 0;
    if (sim_fail_kind == CUT_ERASE && !is_erase) return 0;
    if (sim_fail_kind == CUT_PROGRAM && is_erase) return 0;
    return --sim_fail_countdown == 0;
}

/* 寫到一半：每個 byte 可能寫完、只有部分 bit 變 0，或還沒寫到 */
static void partial_program(uint32_t addr, const uint8_t *data, uint16_t len, int mode)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t *m = &sim_mem[addr + i];
        switch (mode) {
        case TORN_HALF:  if (i < len / 2u) *m &= data[i]; break;
        case TORN_NONE:  break;
        default: {
            uint32_t r = sim_rand() % 4u;
            if (r == 0)      *m &= data[i];                              /* 這個 byte 寫完了 */
            else if (r == 1) *m &= (uint8_t)(data[i] | sim_rand());      /* 只有部分 bit 變 0 */
            /* 其他：還沒寫到 */
        }
        }
    }
}

/* 擦到一半：每個 byte 可能已變回 FF、還沒動、只有部分 bit 變回 1，或停在擦除前的預寫 0 */
static void partial_erase(uint32_t addr)
{
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
        uint8_t *m = &sim_mem[addr + i];
        switch (sim_rand() % 5u) {
        case 0:  *m = 0xFF; break;
        case 1:  break;
        case 2:  *m |= (uint8_t)sim_rand(); break;
        case 3:  *m &= (uint8_t)sim_rand(); break;
        default: *m = 0x00; break;
        }
    }
}

fl_status_t flash_init(SPI_HandleTypeDef *hspi) { (void)hspi; return FL_OK; }

void flash_read_jedec_id(uint8_t *id) { id[0] = 0x0B; id[1] = 0x40; id[2] = 0x18; }

uint8_t flash_read_status(void) { return 0; }

void flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (addr + i < SIM_SIZE) ? sim_mem[addr + i] : 0xFF;
}

fl_status_t flash_page_program(uint32_t addr, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > FLASH_PAGE_SIZE) return FL_ERR_PARAM;
    if (((addr % FLASH_PAGE_SIZE) + len) > FLASH_PAGE_SIZE) return FL_ERR_PARAM;
    if (addr + len > SIM_SIZE) return FL_ERR_PARAM;

    if (addr + len > SIM_ALLOWED) sim_outside_writes++;
    if (sim_weak[addr / FLASH_SECTOR_SIZE]) sim_weak_writes++;   /* 寫進沒擦完的 sector */
    sim_ops++;

    if (should_fail(addr, 0)) {
        partial_program(addr, data, len, TORN_RANDOM);
        sim_fail_happened = 1;
        sim_fails++;
        return FL_ERR_TIMEOUT;
    }

    if (should_cut(0)) {
        partial_program(addr, data, len, sim_torn_mode);   /* 寫到一半就斷電 */
        sim_cut_happened = 1;
        longjmp(sim_cut_jmp, 1);
    }

    for (uint32_t i = 0; i < len; i++) sim_mem[addr + i] &= data[i];
    return FL_OK;
}

fl_status_t flash_sector_erase(uint32_t addr)
{
    addr -= addr % FLASH_SECTOR_SIZE;
    if (addr + FLASH_SECTOR_SIZE > SIM_SIZE) return FL_ERR_PARAM;
    if (addr + FLASH_SECTOR_SIZE > SIM_ALLOWED) sim_outside_writes++;
    sim_ops++;

    uint32_t sec = addr / FLASH_SECTOR_SIZE;

    if (should_fail(addr, 1)) {
        partial_erase(addr);
        sim_weak[sec] = 1;
        sim_fail_happened = 1;
        sim_fails++;
        return FL_ERR_TIMEOUT;
    }

    if (should_cut(1)) {
        int looks_erased = in_log_area(addr) &&
            (sim_weak_mode == WEAK_ALWAYS || (sim_weak_mode == WEAK_RANDOM && (sim_rand() & 1u)));
        if (looks_erased) {                 /* 讀起來全是 FF，但沒擦完 */
            memset(&sim_mem[addr], 0xFF, FLASH_SECTOR_SIZE);
            sim_weak_cuts++;
        } else {
            partial_erase(addr);
        }
        if (in_log_area(addr)) sim_weak[sec] = 1;
        sim_cut_happened = 2;               /* 2 = 斷在擦除中途 */
        longjmp(sim_cut_jmp, 1);
    }

    memset(&sim_mem[addr], 0xFF, FLASH_SECTOR_SIZE);
    sim_weak[sec] = 0;
    sim_erases++;
    return FL_OK;
}
