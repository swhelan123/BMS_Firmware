/* board_i2c.c — STM32F303 I2C2 driver (PA9=SCL, PA10=SDA, AF4).
 *
 * Clock source: HSI (8 MHz) selected via RCC_CFGR3 bit 5 = 0.
 * TIMINGR:      0x00201D2B — 100 kHz standard mode at HSI 8 MHz.
 *               (ST AN4235, Sm 100 kHz, f_I2CCLK=8MHz, tr=1µs, tf=300ns)
 * Addressing:   7-bit. SADD[7:1] = dev_addr; R/W set by I2C_CR2_RD_WRN.
 * Pull-ups:     External 4.75 kΩ to +3V3 on both lines — no internal pull.
 * Error return: BMS_ERR_I2C on NACK/bus error; BMS_ERR_TIMEOUT on loop expiry.
 *
 * Timeout constant: 10 000 polling iterations ≈ 1.4 ms at 72 MHz SYSCLK,
 * which is well above the worst-case 100 kHz transaction time (~0.3 ms for
 * a 3-byte write or 2+2-byte read).
 */
#include "board_i2c.h"
#include "board_pins.h"
#include "bms_hal.h"

#define I2C_TIMEOUT_LOOPS   10000u

/* TIMINGR for 100 kHz standard mode at HSI 8 MHz (ST AN4235 reference value).
 * PRESC=0, SCLDEL=2, SDADEL=0, SCLH=29 (0x1D), SCLL=43 (0x2B). */
#define I2C_TIMINGR_100KHZ  (0x00201D2Bu)

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static BmsResult i2c_wait_flag(uint32_t flag) {
    for (uint32_t n = 0u; n < I2C_TIMEOUT_LOOPS; n++) {
        uint32_t isr = I2C2->ISR;
        if (isr & (I2C_ISR_NACKF | I2C_ISR_BERR | I2C_ISR_ARLO)) {
            I2C2->ICR = I2C_ICR_NACKCF | I2C_ICR_BERRCF
                      | I2C_ICR_ARLOCF | I2C_ICR_STOPCF;
            return BMS_ERR_I2C;
        }
        if (isr & flag) { return BMS_OK; }
    }
    return BMS_ERR_TIMEOUT;
}

static BmsResult i2c_wait_not_busy(void) {
    for (uint32_t n = 0u; n < I2C_TIMEOUT_LOOPS; n++) {
        if (!(I2C2->ISR & I2C_ISR_BUSY)) { return BMS_OK; }
    }
    return BMS_ERR_TIMEOUT;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void board_i2c_init(void) {
    /* Enable peripheral clocks */
    RCC->AHBENR  |= RCC_AHBENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C2EN;

    /* HSI as I2C2 clock source: RCC_CFGR3 bit 5 = 0 (= HSI, default) */
    RCC->CFGR3 &= ~(1u << 5u);

    /* PA9 (SCL) / PA10 (SDA): AF4, open-drain, low speed, no internal pull.
     * External 4.75 kΩ pull-ups to +3V3 are fitted on the board. */
    uint32_t scl_2 = I2C_SCL_PIN * 2u;   /* PA9  → bit offset 18 in MODER/OSPEEDR/PUPDR */
    uint32_t sda_2 = I2C_SDA_PIN * 2u;   /* PA10 → bit offset 20 */
    I2C_PORT->MODER  &= ~((3u << scl_2) | (3u << sda_2));
    I2C_PORT->MODER  |=  (GPIO_MODER_AF << scl_2) | (GPIO_MODER_AF << sda_2);
    I2C_PORT->OTYPER |=  (1u << I2C_SCL_PIN) | (1u << I2C_SDA_PIN);
    I2C_PORT->OSPEEDR &= ~((3u << scl_2) | (3u << sda_2));   /* 00 = low speed */
    I2C_PORT->PUPDR  &= ~((3u << scl_2) | (3u << sda_2));    /* no pull */

    /* AFR[1] controls pins 8–15; index = pin - 8 */
    uint32_t scl_af = (I2C_SCL_PIN - 8u) * 4u;   /* PA9  → bit offset 4 in AFR[1] */
    uint32_t sda_af = (I2C_SDA_PIN - 8u) * 4u;   /* PA10 → bit offset 8 in AFR[1] */
    I2C_PORT->AFR[1] &= ~((0xFu << scl_af) | (0xFu << sda_af));
    I2C_PORT->AFR[1] |=  (I2C_AF << scl_af) | (I2C_AF << sda_af);

    /* Configure I2C2 */
    I2C2->CR1     = 0u;                    /* disable peripheral to allow config */
    I2C2->TIMINGR = I2C_TIMINGR_100KHZ;
    I2C2->CR1     = I2C_CR1_PE;            /* enable */
}

BmsResult board_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t *buf, uint8_t len) {
    if (len == 0u) { return BMS_ERR_INVALID_ARG; }

    BmsResult r = i2c_wait_not_busy();
    if (r != BMS_OK) { return r; }

    /* Phase 1: write-phase — send register address pointer (1 byte, no AUTOEND).
     * Hardware generates a repeated-START when we issue the read-phase next. */
    I2C2->CR2 = ((uint32_t)(dev_addr << 1u) & I2C_CR2_SADD_Msk) |
                (1u << I2C_CR2_NBYTES_Pos) |
                I2C_CR2_START;

    r = i2c_wait_flag(I2C_ISR_TXIS);
    if (r != BMS_OK) { return r; }
    I2C2->TXDR = reg_addr;

    r = i2c_wait_flag(I2C_ISR_TC);   /* TC fires when NBYTES sent, AUTOEND not set */
    if (r != BMS_OK) { return r; }

    /* Phase 2: read-phase — repeated START, read len bytes (AUTOEND → STOP). */
    I2C2->CR2 = ((uint32_t)(dev_addr << 1u) & I2C_CR2_SADD_Msk) |
                ((uint32_t)len << I2C_CR2_NBYTES_Pos) |
                I2C_CR2_AUTOEND |
                I2C_CR2_RD_WRN |
                I2C_CR2_START;

    for (uint8_t i = 0u; i < len; i++) {
        r = i2c_wait_flag(I2C_ISR_RXNE);
        if (r != BMS_OK) { return r; }
        buf[i] = (uint8_t)(I2C2->RXDR & 0xFFu);
    }

    r = i2c_wait_flag(I2C_ISR_STOPF);
    if (r != BMS_OK) { return r; }
    I2C2->ICR = I2C_ICR_STOPCF;

    return BMS_OK;
}

BmsResult board_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr,
                               const uint8_t *data, uint8_t len) {
    BmsResult r = i2c_wait_not_busy();
    if (r != BMS_OK) { return r; }

    /* Write register address then data bytes in a single transfer (AUTOEND). */
    I2C2->CR2 = ((uint32_t)(dev_addr << 1u) & I2C_CR2_SADD_Msk) |
                (((uint32_t)len + 1u) << I2C_CR2_NBYTES_Pos) |
                I2C_CR2_AUTOEND |
                I2C_CR2_START;

    r = i2c_wait_flag(I2C_ISR_TXIS);
    if (r != BMS_OK) { return r; }
    I2C2->TXDR = reg_addr;

    for (uint8_t i = 0u; i < len; i++) {
        r = i2c_wait_flag(I2C_ISR_TXIS);
        if (r != BMS_OK) { return r; }
        I2C2->TXDR = data[i];
    }

    r = i2c_wait_flag(I2C_ISR_STOPF);
    if (r != BMS_OK) { return r; }
    I2C2->ICR = I2C_ICR_STOPCF;

    return BMS_OK;
}
