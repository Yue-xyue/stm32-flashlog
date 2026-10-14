/* stubs.c — 板子上才有的函式，在 PC 上給最簡單的替代 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include "sim.h"

int sim_verbose = 0;

uint32_t HAL_GetTick(void)
{
    static uint32_t t;
    return t += 3;
}

void     perf_init(void) { }
uint32_t perf_cycles(void) { return 0; }
uint32_t perf_us_since(uint32_t start) { (void)start; return 0; }

void log_printf(const char *fmt, ...)
{
    if (!sim_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void dump_hex(const char *tag, const uint8_t *buf, uint32_t len)
{
    (void)tag; (void)buf; (void)len;
}
