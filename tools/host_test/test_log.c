/* test_log.c — 在 PC 上測 log.c 的斷電恢復
 *
 *   demo       ：照板子上的指令跑一次「寫到一半 → 重開 → 寫一筆 → 再重開」，印出 log 的輸出
 *   scenarios  ：保證範圍表上的每一種情況各跑一次
 *   torture N  ：N 組隨機測試；每組連續寫入、在隨機一次 flash 操作中途斷電、重開機後檢查，重複 30 輪
 *
 * 檢查不靠 log.c 自己的判斷：直接在 flash 映像裡逐 byte 找「magic 對、CRC32 也對」的 record，
 * 這就是 flash 上真的還留著的資料，再跟「log_append 回傳成功的資料」比對。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "wear.h"
#include "crc32.h"
#include "sim.h"

#define MAXREC 20000

typedef struct {
    uint16_t len;
    uint8_t  acked;    /* log_append 回傳成功 */
    int      cycle;    /* 第幾輪寫的 */
} mrec_t;

/* 用 seq（測試自己的流水號，寫在 payload 前 4 bytes）當索引 */
static mrec_t   model[MAXREC + 1];
static uint32_t g_seq;
static int      g_cycle;

/* flash 上實際找到的 record */
typedef struct { uint32_t addr, id, seq; uint16_t len; } found_t;
static found_t  found[8192];
static int      nfound;
static int32_t  seq_found[MAXREC + 1];     /* seq -> found 的索引，-1 = 不在 flash 上 */

static char     g_fail[256];

/* ------------------------------------------------------------------ 寫入與重開 */

static void make_payload(uint8_t *p, uint32_t seq, uint16_t len)
{
    memcpy(p, &seq, 4);
    for (uint16_t i = 4; i < len; i++) p[i] = (uint8_t)(seq * 31u + i * 7u + 0x5Au);
}

static log_status_t do_append(uint16_t len)
{
    uint8_t p[LOG_MAX_PAYLOAD];
    uint32_t seq = ++g_seq;
    make_payload(p, seq, len);
    model[seq].len   = len;
    model[seq].acked = 0;
    model[seq].cycle = g_cycle;
    log_status_t rc = log_append(p, len);
    if (rc == LOG_OK) model[seq].acked = 1;
    return rc;
}

static void reboot(void)
{
    sim_cut_countdown = 0;
    wear_init();
    log_init();
}

/* 連續寫 n 筆；中途斷電就回傳 1（等於板子斷電），寫入失敗回傳 -1 */
static int append_many(int n, int mode)
{
    if (setjmp(sim_cut_jmp)) return 1;
    for (volatile int i = 0; i < n; i++) {
        uint16_t len;
        if (mode == 1)      len = 44;                                /* 64 B 一筆：剛好填滿 sector */
        else if (mode == 2) len = 64;
        else                len = (uint16_t)(4 + sim_rand() % 61u);  /* 4..64 */
        if (do_append(len) != LOG_OK) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ 檢查 */

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, &sim_mem[a], 4); return v; }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, &sim_mem[a], 2); return v; }

static void scan_flash(void)
{
    nfound = 0;
    for (uint32_t s = 0; s <= g_seq && s <= MAXREC; s++) seq_found[s] = -1;

    for (uint32_t a = LOG_AREA_START; a + sizeof(rec_header_t) <= LOG_AREA_END; a++) {
        if (rd32(a) != LOG_MAGIC) continue;
        uint16_t len = rd16(a + 8);
        if (len < 4 || len > LOG_MAX_PAYLOAD) continue;
        if (a / FLASH_SECTOR_SIZE != (a + sizeof(rec_header_t) + len - 1) / FLASH_SECTOR_SIZE) continue;
        uint32_t crc = crc32_update(0xFFFFFFFFu, &sim_mem[a], 16);
        crc = crc32_update(crc, &sim_mem[a + 20], len) ^ 0xFFFFFFFFu;
        if (crc != rd32(a + 16)) continue;
        if (nfound >= (int)(sizeof(found) / sizeof(found[0]))) break;
        found_t *f = &found[nfound++];
        f->addr = a;
        f->id   = rd32(a + 4);
        f->len  = len;
        f->seq  = rd32(a + 20);
    }
}

static int fail(const char *msg, unsigned a, unsigned b)
{
    snprintf(g_fail, sizeof(g_fail), msg, a, b);
    return 0;
}

/* window：這一輪和前 window-1 輪寫入成功的，都必須還在 */
static int verify(int window, uint32_t skip_seq)
{
    if (sim_outside_writes) return fail("寫到 log 區外面 %u 次%u", (unsigned)sim_outside_writes, 0);

    scan_flash();

    for (int i = 0; i < nfound; i++) {
        found_t *f = &found[i];
        if (f->seq == 0 || f->seq > g_seq) return fail("flash 上出現沒寫過的 record @0x%06X seq=%u", f->addr, f->seq);
        uint8_t want[LOG_MAX_PAYLOAD];
        make_payload(want, f->seq, f->len);
        if (f->len != model[f->seq].len || memcmp(want, &sim_mem[f->addr + 20], f->len) != 0)
            return fail("record 內容不對 @0x%06X seq=%u", f->addr, f->seq);
        if (seq_found[f->seq] >= 0)
            return fail("同一筆資料出現兩次 seq=%u @0x%06X", f->seq, f->addr);
        seq_found[f->seq] = i;
    }

    /* 編號：寫入順序越後面，rec_id 一定越大（不能重複、不能倒退） */
    uint32_t last_id = 0, last_seq = 0;
    for (uint32_t s = 1; s <= g_seq; s++) {
        if (seq_found[s] < 0) continue;
        uint32_t id = found[seq_found[s]].id;
        if (last_seq && id <= last_id) return fail("rec_id 倒退或重複：seq=%u id=%u", s, id);
        last_id = id; last_seq = s;
    }

#ifdef HAVE_NEW_COMMANDS
    /* log.c 自己數到的有效筆數，不能比 flash 上實際找得到的還多（多了代表重複走訪） */
    {
        uint32_t v, c;
        log_get_counts(&v, &c);
        if (v > (uint32_t)nfound) return fail("log.c 數到 %u 筆有效，但 flash 上只有 %u 筆", v, (unsigned)nfound);
    }
#endif

    /* 最近幾輪寫入成功的，都必須還在 flash 上，而且 log_read 讀得到 */
    int checked = 0;
    for (uint32_t s = g_seq; s >= 1; s--) {
        if (model[s].cycle <= g_cycle - window) break;
        if (!model[s].acked || s == skip_seq) continue;
        if (seq_found[s] < 0) return fail("寫入成功的資料不見了：seq=%u（第 %u 輪）", s, (unsigned)model[s].cycle);
        if (checked < 6 && (checked < 2 || sim_rand() % 8u == 0)) {
            found_t *f = &found[seq_found[s]];
            uint8_t buf[LOG_MAX_PAYLOAD];
            uint16_t n = LOG_MAX_PAYLOAD;
            if (log_read(f->id, buf, &n) != LOG_OK || n != f->len || memcmp(buf, &sim_mem[f->addr + 20], n) != 0)
                return fail("log_read 讀不到 flash 上還在的資料：id=%u seq=%u", f->id, s);
            checked++;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ 情境測試 */

static void fresh(uint32_t seed)
{
    sim_reset(seed);
    g_seq = 0;
    g_cycle = 0;
    memset(model, 0, sizeof(model));
    reboot();
}

static uint32_t seq_addr(uint32_t seq)
{
    scan_flash();
    for (int i = 0; i < nfound; i++) if (found[i].seq == seq) return found[i].addr;
    return 0;
}

/* 1. 寫到一半斷電，重開一次 */
static int sc_torn_once(void)
{
    fresh(1);
    append_many(2, 0);
    sim_cut_kind = CUT_PROGRAM; sim_torn_mode = TORN_HALF; sim_cut_countdown = 1;
    g_cycle++; append_many(1, 0); reboot();
    g_cycle++; if (append_many(3, 0)) return fail("重開後寫入失敗%u%u", 0, 0);
    reboot();
    return verify(3, 0);
}

/* 2. 再重開一次：壞掉那筆還留在 flash 上 */
static int sc_second_reboot(void)
{
    fresh(2);
    append_many(2, 0);
    sim_cut_kind = CUT_PROGRAM; sim_torn_mode = TORN_HALF; sim_cut_countdown = 1;
    g_cycle++; append_many(1, 0); reboot();
    g_cycle++; append_many(1, 0); reboot();        /* 寫 good，再重開 */
    g_cycle++; append_many(1, 0); reboot();        /* 寫 next，再重開 */
    return verify(3, 0);
}

/* 3. 中間某筆壞掉（位元翻轉）：只能丟掉那一筆，後面的都要在 */
static int sc_middle_corrupt(void)
{
    fresh(3);
    for (int i = 0; i < 100; i++) do_append(20);
    uint32_t a = seq_addr(20);
    sim_mem[a + 25] ^= 0x10;                         /* 第 20 筆的 payload 翻一個 bit */
    g_cycle++; reboot();
    g_cycle++; append_many(5, 0); reboot();
    /* 除了第 20 筆，前面 100 筆和後來的 5 筆都必須還在 */
    if (!verify(1, 0)) return 0;
    for (uint32_t s = 1; s <= g_seq; s++)
        if (s != 20 && seq_found[s] < 0) return fail("壞掉的那筆之後的資料不見了：seq=%u%u", s, 0);
    return 1;
}

/* 4. 繞過一圈後，寫到一半斷電 */
static int sc_wrap_torn(void)
{
    fresh(4);
    for (int i = 0; i < 1300; i++) do_append(40);
    sim_cut_kind = CUT_PROGRAM; sim_torn_mode = TORN_HALF; sim_cut_countdown = 1;
    g_cycle++; append_many(1, 0); reboot();
    g_cycle++; append_many(10, 0); reboot();
    g_cycle++; append_many(10, 0); reboot();
    return verify(2, 0);
}

/* 5. 換 sector 時斷電：PAD 寫完，下一個 sector 還沒寫 */
static int sc_pad_cut(void)
{
    fresh(5);
    for (int i = 0; i < 48; i++) do_append(64);    /* 48 × 84 B = 4032 B，第 49 筆放不下 */
    sim_cut_kind = CUT_ANY; sim_torn_mode = TORN_NONE; sim_cut_countdown = 2;   /* 第 1 次是 PAD，第 2 次斷電 */
    g_cycle++; append_many(1, 2); reboot();
    g_cycle++; do_append(10); reboot();            /* 30 B 的一筆放得進剩下的 64 B，舊版會疊寫在 PAD 上 */
    g_cycle++; append_many(3, 0); reboot();
    return verify(2, 0);
}

/* 5b. 中間某筆的 header 壞掉：只能丟掉那個 sector 剩下的，之後的 sector 和新寫的都要在 */
static int sc_middle_header(void)
{
    fresh(8);
    for (int i = 0; i < 400; i++) do_append(20);    /* 40 B 一筆，約 4 個 sector */
    uint32_t a = seq_addr(20);
    sim_mem[a + 5] ^= 0x01;                          /* 第 20 筆的 rec_id 翻一個 bit */
    uint32_t sec_end = (a / FLASH_SECTOR_SIZE + 1) * FLASH_SECTOR_SIZE;
    uint32_t lost_until = 20;
    for (uint32_t s = 21; s <= 400; s++) {           /* 和第 20 筆同一個 sector 的，允許跟著遺失 */
        uint32_t sa = seq_addr(s);
        if (sa && sa < sec_end) lost_until = s;
    }
    g_cycle++; reboot();
    g_cycle++; append_many(5, 0); reboot();
    if (!verify(1, 0)) return 0;
    for (uint32_t s = 1; s <= g_seq; s++)
        if ((s < 20 || s > lost_until) && seq_found[s] < 0) return fail("不該遺失的資料不見了：seq=%u%u", s, 0);
    return 1;
}

/* 6. 擦到一半斷電（繞一圈後回收最舊的 sector 時） */
static int sc_half_erase(void)
{
    int fails = 0;
    for (uint32_t seed = 600; seed < 700; seed++) {
        fresh(seed);
        for (int i = 0; i < 16 * 48; i++) do_append(64);   /* 16 個 sector 剛好寫滿，下一次換 sector 要擦 */
        sim_cut_kind = CUT_ERASE; sim_cut_countdown = 1;
        g_cycle++; append_many(5, 2); reboot();
        g_cycle++; if (append_many(10, 0)) { fails++; continue; }
        reboot();
        g_cycle++; append_many(10, 0); reboot();
        if (!verify(2, 0)) fails++;
    }
    if (fails) return fail("100 次裡有 %u 次出錯%u", (unsigned)fails, 0);
    return 1;
}

/* 7. 剛好填滿 sector（不是斷電）：64 B 一筆，寫超過一圈 */
static int sc_exact_fill(void)
{
    fresh(7);
    if (append_many(16 * 64 + 40, 1)) return fail("寫入失敗%u%u", 0, 0);
    reboot();
    g_cycle = 5;
    if (append_many(5, 1)) return fail("寫入失敗%u%u", 0, 0);
    reboot();
    if (!verify(1, 0)) return 0;
    /* 繞一圈只會回收最舊的 sector：最近 14 個 sector 的資料（896 筆）都必須還在 */
    for (uint32_t s = g_seq - 5 - 896 + 1; s <= g_seq; s++)
        if (seq_found[s] < 0) return fail("寫入成功的資料不見了：seq=%u%u", s, 0);
    return 1;
}

static void run_scenarios(void)
{
    struct { const char *name; int (*fn)(void); } sc[] = {
        { "寫到一半斷電，重開一次",         sc_torn_once },
        { "再重開一次（壞掉那筆還在）",     sc_second_reboot },
        { "中間某筆壞掉（位元翻轉）",       sc_middle_corrupt },
        { "繞過一圈後，寫到一半斷電",       sc_wrap_torn },
        { "換 sector 時斷電（PAD 之後）",   sc_pad_cut },
        { "中間某筆的 header 壞掉",         sc_middle_header },
        { "擦到一半斷電（100 種擦法）",     sc_half_erase },
        { "剛好填滿 sector（不是斷電）",    sc_exact_fill },
    };
    int n = (int)(sizeof(sc) / sizeof(sc[0])), pass = 0;
    for (int i = 0; i < n; i++) {
        g_fail[0] = '\0';
        int ok = sc[i].fn();
        pass += ok;
        printf("  %-32s %s%s%s\n", sc[i].name, ok ? "PASS" : "FAIL", ok ? "" : "  ", ok ? "" : g_fail);
    }
    printf("  情境測試：%d/%d 通過\n", pass, n);
}

/* ------------------------------------------------------------------ 隨機斷電 */

static void run_torture(int seeds)
{
    long cuts = 0, reboots = 0, fails = 0, erase_cuts = 0;
    char first_fail[300] = "";

    for (int seed = 1; seed <= seeds; seed++) {
        fresh(1000u + (uint32_t)seed);
        int mode = (int)(sim_rand() % 4u);        /* 0、3：隨機長度；1：64 B 一筆（剛好填滿）；2：84 B 一筆 */
        if (mode == 3) mode = 0;
        int ok = 1;
        for (g_cycle = 1; g_cycle <= 30 && ok; g_cycle++) {
            int n = 1 + (int)(sim_rand() % 80u);
            int r = (int)(sim_rand() % 10u);
            if (r < 6) {                                   /* 寫入或擦除中途斷電 */
                sim_cut_kind = CUT_ANY; sim_torn_mode = TORN_RANDOM;
                sim_cut_countdown = 1 + (long)(sim_rand() % (uint32_t)(n * 2 + 2));
            } else if (r < 8) {                            /* 專挑擦除的時候斷電 */
                sim_cut_kind = CUT_ERASE; sim_cut_countdown = 1;
            } else {
                sim_cut_countdown = 0;                     /* 正常重開機 */
            }
            sim_cut_happened = 0;
            int rc = append_many(n, mode);
            if (rc < 0) { ok = fail("重開後寫入失敗（第 %u 輪）%u", (unsigned)g_cycle, 0); break; }
            if (sim_cut_happened) { cuts++; if (sim_cut_kind == CUT_ERASE) erase_cuts++; }
            reboot();
            reboots++;
            ok = verify(3, 0);
        }
        if (!ok) {
            fails++;
            if (!first_fail[0]) snprintf(first_fail, sizeof(first_fail), "seed %d：%s", seed, g_fail);
        }
    }
    printf("  隨機斷電：%d 組 × 30 輪，斷電 %ld 次（其中擦除中途 %ld 次），重開機 %ld 次\n",
           seeds, cuts, erase_cuts, reboots);
    printf("  出錯的組數：%ld / %d%s%s\n", fails, seeds, fails ? "，第一個：" : "", first_fail);
}

/* ------------------------------------------------------------------ demo */

static void run_demo(void)
{
    sim_reset(42);
    sim_verbose = 1;
    wear_init();
    log_init();
    const char *cmds[] = { "log format", "log write hello", "log write world", "log partial abcdefghij",
                           "(reset)", "log write good", "(reset)", "log write next", "log dump" };
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        printf("FlashLog> %s\n", cmds[i]);
        if (i == 0) log_format();
        else if (i == 1) log_append((const uint8_t *)"hello", 5);
        else if (i == 2) log_append((const uint8_t *)"world", 5);
        else if (i == 3) log_inject_partial((const uint8_t *)"abcdefghij", 10);
        else if (i == 4 || i == 6) { wear_init(); log_init(); }
        else if (i == 5) log_append((const uint8_t *)"good", 4);
        else if (i == 7) log_append((const uint8_t *)"next", 4);
        else if (i == 8) log_dump(0, 20);
    }
    sim_verbose = 0;
}

/* ------------------------------------------------------------------ 板子上的測試步驟（預期輸出） */

#ifdef HAVE_NEW_COMMANDS
static void cmd(const char *line)
{
    printf("FlashLog> %s\n", line);
}

static void run_hw(void)
{
    sim_reset(7);
    sim_verbose = 1;
    wear_init(); log_init();

    printf("\n---- A. 寫到一半 → 重開 → 寫一筆 → 再重開\n");
    cmd("log format");             log_format();
    cmd("log write hello");        log_append((const uint8_t *)"hello", 5);
    cmd("log write world");        log_append((const uint8_t *)"world", 5);
    cmd("log partial abcdefghij"); log_inject_partial((const uint8_t *)"abcdefghij", 10);
    cmd("(reset)");                wear_init(); log_init();
    cmd("log write good");         log_append((const uint8_t *)"good", 4);
    cmd("(reset)");                wear_init(); log_init();
    cmd("log write next");         log_append((const uint8_t *)"next", 4);
    cmd("log dump");               log_dump(0, 20);

    printf("\n---- B. 換 sector 時，PAD 寫完就斷電\n");
    cmd("log format");             log_format();
    cmd("log fill 48 64");         log_fill(48, 64);
    cmd("log padcut");             log_inject_pad_cut();
    cmd("(reset)");                wear_init(); log_init();
    cmd("log write short");        log_append((const uint8_t *)"short", 5);
    cmd("(reset)");                wear_init(); log_init();
    cmd("log dump 47 5");          log_dump(47, 5);

    printf("\n---- C. 下一個 sector 不是全 FF（模擬擦到一半）\n");
    cmd("log format");             log_format();
    cmd("log fill 70 32");         log_fill(70, 32);
    cmd("write 0x002000 garbage"); flash_page_program(0x002000, (const uint8_t *)"garbage", 7);
    cmd("log fill 20 32");         log_fill(20, 32);
    cmd("log stats");              log_stats();
    cmd("log dump 76 5");          log_dump(76, 5);

    printf("\n---- D. 剛好填滿 sector，並繞過一圈\n");
    cmd("log format");             log_format();
    cmd("log fill 1100 44");       log_fill(1100, 44);
    cmd("log stats");              log_stats();
    cmd("(reset)");                wear_init(); log_init();
    cmd("log stats");              log_stats();

    printf("\n---- E. header 寫到一半\n");
    cmd("log format");             log_format();
    cmd("log write hello");        log_append((const uint8_t *)"hello", 5);
    cmd("log tornhdr");            log_inject_torn_header();
    cmd("(reset)");                wear_init(); log_init();
    cmd("log write after");        log_append((const uint8_t *)"after", 5);
    cmd("(reset)");                wear_init(); log_init();
    cmd("log dump");               log_dump(0, 20);
    sim_verbose = 0;
}
#endif

int main(int argc, char **argv)
{
    const char *what = (argc > 1) ? argv[1] : "all";
    int seeds = (argc > 2) ? atoi(argv[2]) : 2000;

    if (!strcmp(what, "demo") || !strcmp(what, "all")) { printf("== demo\n"); run_demo(); }
    if (!strcmp(what, "scenarios") || !strcmp(what, "all")) { printf("== 情境測試\n"); run_scenarios(); }
    if (!strcmp(what, "torture") || !strcmp(what, "all")) { printf("== 隨機斷電\n"); run_torture(seeds); }
#ifdef HAVE_NEW_COMMANDS
    if (!strcmp(what, "hw")) run_hw();
#endif
    return 0;
}
