/* board_clock.c — HSE + PLL clock configuration for STM32F303VC at 72 MHz. */
#include "board_clock.h"
#include "board_pins.h"
#include "bms_hal.h"

static volatile uint32_t s_tick_ms;

/* ── SysTick handler ──────────────────────────────────────────────────────── */
void SysTick_Handler(void) {
    s_tick_ms++;
}

/* ── Clock init ───────────────────────────────────────────────────────────── */
void board_clock_init(void) {
    /* 1. Enable HSE (8 MHz crystal on PF0/PF1) */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) { /* spin */ }

    /* 2. Flash latency: 2 wait states required for 72 MHz with Vdd 3.3V */
    FLASH_REGS->ACR = (FLASH_REGS->ACR & ~FLASH_ACR_LATENCY_Msk) | 2u;

    /* 3. Configure PLL: HSE × 9 = 72 MHz
     *    CFGR.PLLSRC = 1 (HSE), PLLMUL = 0111 (×9), HPRE = 0 (no div),
     *    PPRE1 = 100 (÷2 → 36 MHz), PPRE2 = 0 (no div → 72 MHz) */
    RCC->CFGR = (RCC->CFGR
                 & ~(0xFFu << 18 | 0xFu << 4 | 0x7u << 8 | 0x7u << 11))
                | (1u << 16)      /* PLLSRC = HSE */
                | (0x7u << 18)    /* PLLMUL = ×9 */
                | (0u << 4)       /* HPRE = /1 (AHB = 72 MHz) */
                | (0x4u << 8)     /* PPRE1 = /2 (APB1 = 36 MHz) */
                | (0u << 11);     /* PPRE2 = /1 (APB2 = 72 MHz) */

    /* 4. Enable PLL */
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { /* spin */ }

    /* 5. Select PLL as system clock */
    RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) { /* spin */ }

    /* 6. Enable GPIO clocks for used ports */
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN |
                   RCC_AHBENR_GPIOCEN | RCC_AHBENR_GPIOFEN;

    /* 7. Enable peripheral clocks.
     * ADC12 is on AHB (not APB2) on STM32F303 — RM0316 §9.4.6. */
    RCC->AHBENR  |= RCC_AHBENR_ADC12EN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN | RCC_APB1ENR_I2C2EN |
                    RCC_APB1ENR_CANEN | RCC_APB1ENR_PWREN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

    /* 8. SysTick at 1 ms (72 MHz / 1000 = 72000 ticks) */
    SysTick->LOAD = 72000u - 1u;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE | SysTick_CTRL_TICKINT | SysTick_CTRL_ENABLE;

    /* 9. Enable the DWT cycle counter for microsecond-resolution busy-waits
     * (board_clock_delay_us). SysTick only gives 1 ms; the isoSPI wake pulse
     * needs ~300 µs, and it runs several times per measurement cycle, so a
     * ms-granular delay there would blow the loop's timing budget. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* 10. Ensure IRQs are enabled regardless of how we were entered.
     * The bootloader jumps here with PRIMASK set unless it re-enables IRQs;
     * without this, SysTick never fires and every delay loop hangs. */
#ifndef BMS_HOST_BUILD
    __asm volatile ("cpsie i" ::: "memory");
#endif
}

uint32_t board_clock_get_ms(void) {
    return s_tick_ms;
}

void board_clock_delay_ms(uint32_t ms) {
    uint32_t start = s_tick_ms;
    while ((s_tick_ms - start) < ms) { /* spin */ }
}

void board_clock_delay_us(uint32_t us) {
    /* DWT cycle counter at the 72 MHz core clock → 72 cycles per µs.
     * Wrap-safe: the unsigned subtraction handles CYCCNT rollover. */
    uint32_t start  = DWT->CYCCNT;
    uint32_t cycles = us * 72u;
    while ((DWT->CYCCNT - start) < cycles) { /* spin */ }
}
