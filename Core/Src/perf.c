/* perf.c — 以 DWT cycle counter 做微秒級量測 */

#include "perf.h"

void perf_init(void)
{
    /* 啟用 trace 單元，DWT 才會運作 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t perf_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t perf_us_since(uint32_t start_cycles)
{
    /* 無號數相減，計數器溢位時也會得到正確的差值 */
    uint32_t elapsed = DWT->CYCCNT - start_cycles;
    return elapsed / (SystemCoreClock / 1000000u);
}
