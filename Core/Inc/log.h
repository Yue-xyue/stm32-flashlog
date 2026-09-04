/*
 * log.h
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

#ifndef INC_LOG_H_
#define INC_LOG_H_

#include <stdint.h>
#include "flash.h"

#define LOG_MAGIC        0x464C5247u   /* "FLRG" */
#define LOG_MAGIC_PAD    0x50414421u   /* "PAD!" — 填補 record */

#define LOG_AREA_START   0x001000u     /* sector 0 保留給 metadata */
#define LOG_SECTOR_COUNT 16
#define LOG_AREA_END     (LOG_AREA_START + LOG_SECTOR_COUNT * FLASH_SECTOR_SIZE)

#define LOG_MAX_PAYLOAD  64

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t rec_id;
    uint16_t length;
    uint16_t reserved;
    uint32_t timestamp;
    uint32_t crc32;
} rec_header_t;

typedef enum {
    LOG_OK = 0,
    LOG_ERR_PARAM   = -1,
    LOG_ERR_FULL    = -2,
    LOG_ERR_IO      = -3,
    LOG_ERR_NOTFOUND= -4,
} log_status_t;

log_status_t log_init(void);
log_status_t log_append(const uint8_t *data, uint16_t len);
log_status_t log_read(uint32_t rec_id, uint8_t *buf, uint16_t *len);
void         log_dump(void);
void         log_stats(void);
log_status_t log_format(void);
log_status_t log_inject_corrupt(void);
log_status_t log_inject_partial(const uint8_t *data, uint16_t len);

uint32_t log_last_append_us(void);
uint32_t log_last_read_us(void);
uint32_t log_last_init_us(void);

#endif /* INC_LOG_H_ */
