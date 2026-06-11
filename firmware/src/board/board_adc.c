/* board_adc.c — ADC1 for Vpack measurement on PA1 (ADC1_IN2).
 *
 * Clock: ADC12 clock via RCC_AHBENR_ADC12EN. PLLCLK/1 as ADCCLK via RCC_CFGR2.
 * Mode: single-conversion, 12-bit, software-triggered.
 */
#include "board_adc.h"
#include "board_pins.h"
#include "board_clock.h"
#include "bms_hal.h"

/* ADC startup timeout (ms) — covers calibration + ready */
#define ADC_STARTUP_TIMEOUT_MS  (10u)

void board_adc_init(void) {
    /* PA1 → analog input mode (MODER[3:2] = 11) */
    VPACK_ADC_PORT->MODER &= ~(3u << (VPACK_ADC_PIN * 2u));
    VPACK_ADC_PORT->MODER |= (GPIO_MODER_ANALOG << (VPACK_ADC_PIN * 2u));

    /* Enable ADC1/2 peripheral clock */
    RCC->AHBENR |= RCC_AHBENR_ADC12EN;

    /* Set ADC12 clock: PLLCLK (not divided) as ADCCLK.
     * RCC_CFGR2 ADCPRE12[8:4] = 10000 → PLLCLK/1 synchronous mode. */
    RCC->CFGR2 &= ~RCC_CFGR2_ADCPRE12;
    RCC->CFGR2 |= RCC_CFGR2_ADCPRE12_DIV1;

    /* ADC voltage regulator enable: ADVREGEN[1:0] must go 00→10→01 */
    ADC1->CR &= ~ADC_CR_ADVREGEN;
    ADC1->CR |= ADC_CR_ADVREGEN_0;
    board_clock_delay_ms(1); /* voltage regulator startup: ≥10 µs per datasheet */

    /* Calibrate in single-ended mode */
    ADC1->CR &= ~ADC_CR_ADCALDIF;
    ADC1->CR |= ADC_CR_ADCAL;
    uint32_t t0 = board_clock_get_ms();
    while ((ADC1->CR & ADC_CR_ADCAL) &&
           (board_clock_get_ms() - t0 < ADC_STARTUP_TIMEOUT_MS)) { }

    /* Enable ADC and wait for ready */
    ADC1->ISR |= ADC_ISR_ADRDY; /* clear ready flag */
    ADC1->CR  |= ADC_CR_ADEN;
    t0 = board_clock_get_ms();
    while (!(ADC1->ISR & ADC_ISR_ADRDY) &&
           (board_clock_get_ms() - t0 < ADC_STARTUP_TIMEOUT_MS)) { }

    /* Configure: 12-bit, single conversion, right-aligned (default CFGR = 0) */
    ADC1->CFGR = 0u;

    /* Sample time for IN2: 601.5 cycles (safe for high-impedance sources).
     * SMPR1 SMP2[8:6] = 111 */
    ADC1->SMPR1 = (7u << ADC_SMPR1_SMP2_Pos);

    /* Regular sequence: 1 conversion, channel 2.
     * SQR1: L[3:0]=0 (1 conv), SQ1[10:6]=2 */
    ADC1->SQR1 = (VPACK_ADC_CHANNEL << ADC_SQR1_SQ1_Pos);
}

BmsResult board_adc_read_raw(uint16_t *raw_out) {
    #define ADC_EOC_TIMEOUT_MS  (5u)

    /* Trigger single software conversion */
    ADC1->CR |= ADC_CR_ADSTART;

    uint32_t t0 = board_clock_get_ms();
    while (!(ADC1->ISR & ADC_ISR_EOC)) {
        if (board_clock_get_ms() - t0 >= ADC_EOC_TIMEOUT_MS) {
            ADC1->CR |= ADC_CR_ADSTP; /* abort conversion */
            return BMS_ERR_TIMEOUT;
        }
    }

    *raw_out = (uint16_t)(ADC1->DR & 0x0FFFu);
    return BMS_OK;
}
