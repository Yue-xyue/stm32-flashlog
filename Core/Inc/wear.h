#ifndef INC_WEAR_H_
#define INC_WEAR_H_

#include <stdint.h>
#include "flash.h"

#define WEAR_MAX_SECTORS  16

typedef struct {
    uint32_t count[WEAR_MAX_SECTORS];
    uint32_t total;
    uint32_t min, max;
    uint32_t events_used;      /* metadata sector 已用幾筆 */
} wear_stats_t;

void wear_init(void);                    /* 開機掃描，重建統計 */
void wear_record_erase(uint32_t sector); /* 記錄一次擦除 */
void wear_get(wear_stats_t *out);
void wear_reset(void);                   /* 擦掉 metadata sector */

#endif
