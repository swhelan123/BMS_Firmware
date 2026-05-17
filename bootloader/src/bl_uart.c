/* bl_uart.c — Bootloader USART2 driver (HSI 8 MHz, 115200 baud, polling).
 *
 * PA2 = USART2_TX (AF7), PA3 = USART2_RX (AF7).
 * At HSI 8 MHz: BRR = 8000000/115200 = 69  (0.65 % error — within USART spec).
 * No DMA, no interrupts; the bootloader processes one frame at a time.
 */
#include "bl_uart.h"
#include <stdint.h>
#include <stdbool.h>

#ifndef BMS_HOST_BUILD

/* ── RCC ──────────────────────────────────────────────────────────────────── */
#define RCC_AHBENR   (*(volatile uint32_t *)0x40021014u)
#define RCC_APB1ENR  (*(volatile uint32_t *)0x4002101Cu)
#define RCC_AHBENR_GPIOAEN    (1u << 17)
#define RCC_APB1ENR_USART2EN  (1u << 17)

/* ── GPIOA ────────────────────────────────────────────────────────────────── */
#define GPIOA_BASE    0x48000000u
#define GPIOA_MODER   (*(volatile uint32_t *)(GPIOA_BASE + 0x00u))
#define GPIOA_AFR0    (*(volatile uint32_t *)(GPIOA_BASE + 0x20u))

/* ── USART2 ───────────────────────────────────────────────────────────────── */
#define USART2_BASE   0x40004400u
#define USART2_CR1    (*(volatile uint32_t *)(USART2_BASE + 0x00u))
#define USART2_BRR    (*(volatile uint32_t *)(USART2_BASE + 0x0Cu))
#define USART2_ISR    (*(volatile uint32_t *)(USART2_BASE + 0x1Cu))
#define USART2_RDR    (*(volatile uint32_t *)(USART2_BASE + 0x24u))
#define USART2_TDR    (*(volatile uint32_t *)(USART2_BASE + 0x28u))

#define USART_CR1_UE  (1u << 0)
#define USART_CR1_RE  (1u << 2)
#define USART_CR1_TE  (1u << 3)
#define USART_ISR_RXNE (1u << 5)
#define USART_ISR_TC   (1u << 6)
#define USART_ISR_TXE  (1u << 7)

#define BL_UART_BRR   69u   /* 8000000 / 115200 */

void bl_uart_init(void) {
    RCC_AHBENR  |= RCC_AHBENR_GPIOAEN;
    RCC_APB1ENR |= RCC_APB1ENR_USART2EN;

    /* PA2 and PA3 → alternate function mode (MODER = 0b10) */
    GPIOA_MODER &= ~(0xFFu << 4);
    GPIOA_MODER |=  (0x02u << 4) | (0x02u << 6);

    /* PA2 = AF7 (bits [11:8]), PA3 = AF7 (bits [15:12]) in AFRL */
    GPIOA_AFR0 &= ~(0xFFu << 8);
    GPIOA_AFR0 |=  (0x07u << 8) | (0x07u << 12);

    USART2_BRR = BL_UART_BRR;
    USART2_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void bl_uart_write(const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0u; i < len; i++) {
        while (!(USART2_ISR & USART_ISR_TXE)) {}
        USART2_TDR = data[i];
    }
    while (!(USART2_ISR & USART_ISR_TC)) {}
}

bool bl_uart_rx_ready(void) {
    return (USART2_ISR & USART_ISR_RXNE) != 0u;
}

uint8_t bl_uart_read_byte(void) {
    return (uint8_t)(USART2_RDR & 0xFFu);
}

#else /* BMS_HOST_BUILD — stubs for unit tests */

void bl_uart_init(void) {}

void bl_uart_write(const uint8_t *data, uint16_t len) {
    (void)data; (void)len;
}

bool bl_uart_rx_ready(void) { return false; }

uint8_t bl_uart_read_byte(void) { return 0u; }

#endif /* BMS_HOST_BUILD */
