/* sim.h — PC 上模擬 SPI NOR flash 的控制介面（給 host 測試用） */
#ifndef HOST_SIM_H
#define HOST_SIM_H

#include <stdint.h>
#include <setjmp.h>

#define SIM_SIZE      0x020000u     /* 模擬 128 KB：wear 區、log 區，加上 log 區外面一段用來抓越界寫入 */
#define SIM_ALLOWED   0x011000u     /* 允許寫入的範圍：[0, LOG_AREA_END) */
#define SIM_LOG_START 0x001000u     /* log 區開頭；「沒擦完」和「回傳失敗」只模擬在 log 區 */
#define SIM_SECTORS   (SIM_SIZE / 4096u)

enum { CUT_ANY = 0, CUT_ERASE = 1, CUT_PROGRAM = 2 };
enum { TORN_RANDOM = 0, TORN_HALF = 1, TORN_NONE = 2 };
enum { WEAK_NEVER = 0, WEAK_ALWAYS = 1, WEAK_RANDOM = 2 };

extern uint8_t  sim_mem[SIM_SIZE];
extern long     sim_ops;            /* 累計的寫入＋擦除次數 */
extern long     sim_erases;
extern long     sim_cut_countdown;  /* >0：再過幾次（符合種類的）操作就斷電；0：不斷電 */
extern int      sim_cut_kind;       /* CUT_ANY / CUT_ERASE / CUT_PROGRAM */
extern int      sim_torn_mode;      /* 寫入斷電時寫進去多少 */
extern int      sim_cut_happened;   /* 1 = 斷在寫入中途，2 = 斷在擦除中途 */
extern long     sim_outside_writes; /* 寫到允許範圍外的次數 */
extern jmp_buf  sim_cut_jmp;
extern int      sim_verbose;

/* 擦除沒做完（斷電或回傳失敗）的 sector 標記為「沒擦完」，要等下一次完整擦除才清掉。
   斷電在擦除中途時，sim_weak_mode 決定它讀起來的樣子：隨機殘留，或「全是 FF 但沒擦完」。
   寫進沒擦完的 sector 就算錯（原廠的規則：擦除被中斷的 sector 要重擦，讀起來是 FF 也一樣）。 */
extern int      sim_weak_mode;      /* WEAK_NEVER / WEAK_ALWAYS / WEAK_RANDOM */
extern uint8_t  sim_weak[SIM_SECTORS];
extern long     sim_weak_cuts;      /* 擦除中途斷電、而且讀起來全是 FF 的次數 */
extern long     sim_weak_writes;    /* 寫進沒擦完的 sector 的次數（必須是 0） */

/* 不斷電，但 flash 回傳失敗（例如 SPI 逾時）：那次寫入或擦除只做一部分，回傳 FL_ERR_TIMEOUT，程式繼續跑 */
extern long     sim_fail_countdown; /* >0：再過幾次 log 區的（符合種類的）操作就回傳失敗 */
extern int      sim_fail_kind;      /* CUT_ANY / CUT_ERASE / CUT_PROGRAM */
extern int      sim_fail_happened;
extern long     sim_fails;          /* 累計回傳失敗的次數 */

void     sim_reset(uint32_t seed);
uint32_t sim_rand(void);

#endif
