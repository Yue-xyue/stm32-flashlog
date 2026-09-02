/*
 * storage.c
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

/* storage.c — task 間共享的 queue 定義 */

#include "storage.h"

osMessageQueueId_t storageQueueHandle;
osMessageQueueId_t uartRxQueueHandle;
