/* bl_uart.h — Minimal bootloader UART driver (USART2, PA2/PA3, HSI 8 MHz). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Init USART2 at 115200 baud (HSI 8 MHz). PA2=TX AF7, PA3=RX AF7. */
void bl_uart_init(void);

/* Blocking byte-for-byte transmit; waits for TC before returning. */
void bl_uart_write(const uint8_t *data, uint16_t len);

/* True if a byte is waiting in USART2->RDR. */
bool bl_uart_rx_ready(void);

/* Read one byte. Call only when bl_uart_rx_ready() is true. */
uint8_t bl_uart_read_byte(void);
