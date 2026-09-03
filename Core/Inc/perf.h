#ifndef INC_PERF_H_
#define INC_PERF_H_

#include <stdint.h>
#include "main.h"

void     perf_init(void);          /* 開機時呼叫一次 */
uint32_t perf_cycles(void);        /* 目前的 CPU 週期數 */
uint32_t perf_us_since(uint32_t start_cycles);

#endif
