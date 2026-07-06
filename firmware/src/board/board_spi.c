/* board_spi.c — SPI1 driver: PA5=SCK, PA6=MISO, PA7=MOSI, mode 3. */
#include "board_spi.h"
#include "board_pins.h"
#include "bms_hal.h"
#include <stddef.h>

static bool s_busy;

/* ── GPIO helpers ─────────────────────────────────────────────────────────── */
static inline void pin_high(GPIO_TypeDef *port, uint32_t pin) {
    port->BSRR = (1u << pin);
}
static inline void pin_low(GPIO_TypeDef *port, uint32_t pin) {
    port->BSRR = (1u << (pin + 16u));
}

void board_spi_init(void) {
    /* PA5 SCK, PA6 MISO, PA7 MOSI → AF5 */
    SPI_PORT->MODER &= ~(0x3Fu << 10);  /* clear pins 5,6,7 */
    SPI_PORT->MODER |= (GPIO_MODER_AF << 10) | (GPIO_MODER_AF << 12) | (GPIO_MODER_AF << 14);
    SPI_PORT->AFR[0] &= ~(0xFFFu << 20);
    SPI_PORT->AFR[0] |= (SPI_AF << 20) | (SPI_AF << 24) | (SPI_AF << 28);

    /* CS_CELL PA4 → output, idle HIGH */
    CS_CELL_PORT->MODER &= ~(0x3u << (CS_CELL_PIN * 2u));
    CS_CELL_PORT->MODER |= (GPIO_MODER_OUTPUT << (CS_CELL_PIN * 2u));
    pin_high(CS_CELL_PORT, CS_CELL_PIN);

    /* CS_TEMP PB12 → output, idle HIGH */
    CS_TEMP_PORT->MODER &= ~(0x3u << (CS_TEMP_PIN * 2u));
    CS_TEMP_PORT->MODER |= (GPIO_MODER_OUTPUT << (CS_TEMP_PIN * 2u));
    pin_high(CS_TEMP_PORT, CS_TEMP_PIN);

    /* SPI1: master, mode 3 (CPOL=1 CPHA=1), software NSS.
     * fPCLK(APB2)=72 MHz. BR=111 → /256 ≈ 281 kHz. The LTC6820's isoSPI
     * SPI port is limited to ~500 kHz because this board straps SLOW=1
     * (all LTC6820 config pins tied high — confirmed from PCB netlist);
     * 281 kHz is safely inside slow-mode spec. Raise toward /128 (562 kHz)
     * only after comms are proven and only if cycle timing needs it. */
    SPI1->CR1 = SPI_CR1_CPOL | SPI_CR1_CPHA | SPI_CR1_MSTR |
                SPI_CR1_SSM | SPI_CR1_SSI | (7u << SPI_CR1_BR_Pos);
    /* 8-bit DS + FRXTH: RXNE must fire per byte (8-bit FIFO threshold).
     * Without FRXTH the F3 SPI FIFO waits for 16 bits before setting RXNE,
     * and the byte-wise transfer loop below hangs on its first byte. */
    SPI1->CR2 = SPI_CR2_FRXTH | 0x0700u;
    SPI1->CR1 |= SPI_CR1_SPE;

    s_busy = false;
}

void board_spi_cs_assert(BmsChain chain) {
    if (chain == BMS_CHAIN_CELL) {
        pin_low(CS_CELL_PORT, CS_CELL_PIN);
    } else {
        pin_low(CS_TEMP_PORT, CS_TEMP_PIN);
    }
    s_busy = true;
}

void board_spi_cs_deassert(BmsChain chain) {
    if (chain == BMS_CHAIN_CELL) {
        pin_high(CS_CELL_PORT, CS_CELL_PIN);
    } else {
        pin_high(CS_TEMP_PORT, CS_TEMP_PIN);
    }
    s_busy = false;
}

void board_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        while (!(SPI1->SR & SPI_SR_TXE)) { /* wait TX */ }
        *((volatile uint8_t *)&SPI1->DR) = tx ? tx[i] : 0xFFu;
        while (!(SPI1->SR & SPI_SR_RXNE)) { /* wait RX */ }
        uint8_t byte = *((volatile uint8_t *)&SPI1->DR);
        if (rx) { rx[i] = byte; }
    }
    while (SPI1->SR & SPI_SR_BSY) { /* drain */ }
}

void board_spi_write(const uint8_t *tx, uint16_t len) {
    board_spi_transfer(tx, NULL, len);
}

bool board_spi_is_busy(void) {
    return s_busy;
}
