/* board_clock.h — clock and systick initialisation. */
#pragma once
#include <stdint.h>

/* Configure HSE → PLL → 72 MHz system clock.
 * APB1 = 36 MHz (max for APB1 peripherals).
 * APB2 = 72 MHz.
 * Flash latency = 2 wait states.
 * Enables GPIO clocks for all used ports.
 * Enables SysTick at 1 ms tick rate. */
void board_clock_init(void);

/* Returns milliseconds since board_clock_init() was called. Wraps at 2^32. */
uint32_t board_clock_get_ms(void);

/* Busy-wait delay. */
void board_clock_delay_ms(uint32_t ms);

/* Microsecond busy-wait via the DWT cycle counter (enabled in
 * board_clock_init). For sub-millisecond timing like the isoSPI wake pulse. */
void board_clock_delay_us(uint32_t us);

/* SysTick ISR — called by the NVIC vector table. Do not call directly. */
void SysTick_Handler(void);
