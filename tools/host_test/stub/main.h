/* host 測試用的假 main.h：只提供 log.c / wear.c 需要的型別和函式宣告 */
#ifndef HOST_STUB_MAIN_H
#define HOST_STUB_MAIN_H
#include <stdint.h>
typedef struct { int dummy; } SPI_HandleTypeDef;
typedef struct { int dummy; } UART_HandleTypeDef;
uint32_t HAL_GetTick(void);
#endif
