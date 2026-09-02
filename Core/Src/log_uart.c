/* log_uart.c — UART 輸出與接收底層 */

#include "log_uart.h"
#include "storage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

osMutexId_t uartMutexHandle;

volatile uint8_t uart_rx_byte;

int __io_putchar(int ch)
{
    while ((USART2->SR & USART_SR_TXE) == 0) { }   /* 等發送暫存器空 */
    USART2->DR = (uint8_t)ch;
    return ch;
}

/* 所有 task 的輸出都走這裡，由 mutex 序列化 */
void log_printf(const char *fmt, ...)
{
    va_list args;
    osMutexAcquire(uartMutexHandle, osWaitForever);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    osMutexRelease(uartMutexHandle);
}

void dump_hex(const char *tag, const uint8_t *buf, uint32_t len)
{
    osMutexAcquire(uartMutexHandle, osWaitForever);   /* 整行一起鎖，不可只鎖單次 printf */
    printf("%s:", tag);
    for (uint32_t i = 0; i < len; i++) printf(" %02X", buf[i]);
    printf("\r\n");
    osMutexRelease(uartMutexHandle);
}

/* 單一字元直送，不經過 stdio 緩衝 */
void uart_putc_raw(char c)
{
    while ((USART2->SR & USART_SR_TXE) == 0) { }
    USART2->DR = (uint8_t)c;
}

void uart_puts_raw(const char *s)
{
    while (*s) uart_putc_raw(*s++);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        uint8_t b = uart_rx_byte;
        osMessageQueuePut(uartRxQueueHandle, &b, 0, 0);  /* 逾時必須為 0：ISR 內不可阻塞 */
        HAL_UART_Receive_IT(huart, (uint8_t *)&uart_rx_byte, 1);  /* 重新啟動接收 */
    }
}

