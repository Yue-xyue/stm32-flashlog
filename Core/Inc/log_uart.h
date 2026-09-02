/*
 * log_uart.h
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */


#ifndef INC_LOG_UART_H_
#define INC_LOG_UART_H_

#include <stdint.h>
#include <stdarg.h>
#include "main.h"
#include "cmsis_os.h"

extern osMutexId_t uartMutexHandle;
extern volatile uint8_t uart_rx_byte;

int __io_putchar(int ch);
void log_printf(const char *fmt, ...);
void dump_hex(const char *tag, const uint8_t *buf, uint32_t len);
void uart_putc_raw(char c);
void uart_puts_raw(const char *s);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);


#endif /* INC_LOG_UART_H_ */
