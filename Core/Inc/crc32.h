/*
 * crc32.h
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

#ifndef INC_CRC32_H_
#define INC_CRC32_H_

#include <stdint.h>

uint32_t crc32_calc(const uint8_t *data, uint32_t len);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

#endif /* INC_CRC32_H_ */
