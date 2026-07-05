/* board_uart.c — USART2 init and IO (PA2 TX, PA3 RX).
 *
 * RX is interrupt-driven into a ring buffer: the main loop spends tens of
 * milliseconds inside measurement cycles (isoSPI timeouts, ADC settling), far
 * longer than one byte time at 115200 baud (~87 µs). Polled RX with the
 * 1-byte RDR guarantees overrun mid-frame; the ISR keeps reception lossless
 * while the read API below stays unchanged for callers.
 * TX remains blocking — responses are sent from one context only. */
#include "board_uart.h"
#include "board_pins.h"
#include "bms_hal.h"
#include "bms_constants.h"

/* ── RX ring buffer (power of two for cheap wrap) ────────────────────────── */
#define UART_RX_BUF_SIZE  512u
#define UART_RX_BUF_MASK  (UART_RX_BUF_SIZE - 1u)

static volatile uint8_t  s_rx_buf[UART_RX_BUF_SIZE];
static volatile uint16_t s_rx_head; /* ISR writes */
static volatile uint16_t s_rx_tail; /* main loop reads */

void USART2_IRQHandler(void) {
    uint32_t isr = USART2->ISR;

    /* Overrun: clear the flag so reception continues; the lost byte will
     * surface as a frame CRC failure and the host will retry. */
    if (isr & USART_ISR_ORE) {
        USART2->ICR = USART_ICR_ORECF;
    }

    while (USART2->ISR & USART_ISR_RXNE) {
        uint8_t byte = (uint8_t)(USART2->RDR & 0xFFu);
        uint16_t next = (uint16_t)((s_rx_head + 1u) & UART_RX_BUF_MASK);
        if (next != s_rx_tail) {          /* drop byte if buffer full */
            s_rx_buf[s_rx_head] = byte;
            s_rx_head = next;
        }
    }
}

void board_uart_init(void) {
    /* PA2 TX, PA3 RX → AF7 */
    /* MODER: pins 2,3 → AF (0x2) */
    UART_PORT->MODER &= ~(0xFu << 4);
    UART_PORT->MODER |= (GPIO_MODER_AF << 4) | (GPIO_MODER_AF << 6);
    /* AFR[0] bits [11:8]=AF7 (pin2), [15:12]=AF7 (pin3) */
    UART_PORT->AFR[0] &= ~(0xFFu << 8);
    UART_PORT->AFR[0] |= (UART_AF << 8) | (UART_AF << 12);

    s_rx_head = 0u;
    s_rx_tail = 0u;

    /* USART2: 115200 baud at PCLK1 = 36 MHz → BRR = 36e6 / 115200 ≈ 313 */
    USART2->BRR = (uint32_t)(36000000u / UART_BAUD_RATE);
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    NVIC_SetPriority(USART2_IRQn, 3u);
    NVIC_EnableIRQ(USART2_IRQn);
}

void board_uart_write(const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        while (!(USART2->ISR & USART_ISR_TXE)) { /* wait for TX empty */ }
        USART2->TDR = data[i];
    }
    /* wait for last byte to shift out */
    while (!(USART2->ISR & USART_ISR_TC)) { /* spin */ }
}

bool board_uart_rx_ready(void) {
    return s_rx_head != s_rx_tail;
}

uint8_t board_uart_read_byte(void) {
    if (s_rx_head == s_rx_tail) { return 0u; }
    uint8_t byte = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) & UART_RX_BUF_MASK);
    return byte;
}

uint16_t board_uart_read(uint8_t *buf, uint16_t max_len) {
    uint16_t n = 0;
    while (n < max_len && board_uart_rx_ready()) {
        buf[n++] = board_uart_read_byte();
    }
    return n;
}
