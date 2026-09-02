/*
 * flash.h
 *
 *  Created on: 2026年7月24日
 *      Author: scj
 */

#ifndef INC_FLASH_H_
#define INC_FLASH_H_

#include <stdint.h>
#include "main.h"

#define FLASH_PAGE_SIZE         256
#define FLASH_SECTOR_SIZE       4096

/* ---- 回傳碼 ---- */
typedef enum {
    FL_OK          = 0,
    FL_ERR_PARAM   = -1,
    FL_ERR_TIMEOUT = -2,
} fl_status_t;

/* ---- 公開 API ---- */
fl_status_t flash_init(SPI_HandleTypeDef *hspi);
void        flash_read_jedec_id(uint8_t *id);              /* id 需 3 bytes */
uint8_t     flash_read_status(void);
fl_status_t flash_sector_erase(uint32_t addr);
fl_status_t flash_page_program(uint32_t addr, const uint8_t *data, uint16_t len);
void        flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

#endif /* INC_FLASH_H_ */
