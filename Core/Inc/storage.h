/*
 * storage.h
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */


#ifndef INC_STORAGE_H_
#define INC_STORAGE_H_

#include <stdint.h>
#include "cmsis_os.h"

typedef enum {
    REQ_ID = 0,
    REQ_ERASE,
    REQ_WRITE,
    REQ_READ,
    REQ_LOG_WRITE,
    REQ_LOG_READ,
    REQ_LOG_DUMP,
    REQ_LOG_STATS,
    REQ_LOG_FORMAT,REQ_LOG_CORRUPT,
	REQ_LOG_PARTIAL,
	REQ_LOG_REMOUNT,
	REQ_LOG_WEAR,
	REQ_WEAR_RESET,
} req_type_t;

typedef struct {
    req_type_t type;
    uint32_t   addr;
    uint16_t   len;
    uint8_t    data[64];
} storage_req_t;


extern osMessageQueueId_t storageQueueHandle;
extern osMessageQueueId_t uartRxQueueHandle;

#endif /* INC_STORAGE_H_ */
