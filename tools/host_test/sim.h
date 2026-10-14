/* sim.h — PC 上模擬 SPI NOR flash 的控制介面（給 host 測試用） */
#ifndef HOST_SIM_H
#define HOST_SIM_H

#include <stdint.h>
#include <setjmp.h>

#define SIM_SIZE      0x020000u     /* 模擬 128 KB：wear 區、log 區，加上 log 區外面一段用來抓越界寫入 */
#define SIM_ALLOWED   0x011000u     /* 允許寫入的範圍：[0, LOG_AREA_END) */

enum { CUT_ANY = 0, CUT_ERASE = 1, CUT_PROGRAM = 2 };
enum { TORN_RANDOM = 0, TORN_HALF = 1, TORN_NONE = 2 };

extern uint8_t  sim_mem[SIM_SIZE];
extern long     sim_ops;            /* 累計的寫入＋擦除次數 */
extern long     sim_erases;
extern long     sim_cut_countdown;  /* >0：再過幾次（符合種類的）操作就斷電；0：不斷電 */
extern int      sim_cut_kind;       /* CUT_ANY / CUT_ERASE / CUT_PROGRAM */
extern int      sim_torn_mode;      /* 寫入斷電時寫進去多少 */
extern int      sim_cut_happened;
extern long     sim_outside_writes; /* 寫到允許範圍外的次數 */
extern jmp_buf  sim_cut_jmp;
extern int      sim_verbose;

void     sim_reset(uint32_t seed);
uint32_t sim_rand(void);

#endif
