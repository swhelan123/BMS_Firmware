/* mock_board_clock.c — mock clock for host tests. */
#include "board_clock.h"
static uint32_t s_tick;
void board_clock_init(void) { s_tick = 0; }
uint32_t board_clock_get_ms(void) { return s_tick; }
void board_clock_delay_ms(uint32_t ms) { s_tick += ms; }
void board_clock_delay_us(uint32_t us) { (void)us; /* no-op in host tests */ }
void SysTick_Handler(void) { s_tick++; }
/* Allow tests to advance time */
void mock_clock_advance_ms(uint32_t ms) { s_tick += ms; }
