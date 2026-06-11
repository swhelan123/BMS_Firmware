/* main.c — BMS application entry point.
 *
 * Init sequence:
 *   1. board_outputs_init_safe() — permission outputs deasserted before anything else
 *   2. board_clock_init() — HSE→PLL 72 MHz, GPIO/peripheral clocks enabled
 *   3. board_uart_init(), board_spi_init(), board_i2c_init(), etc.
 *   4. bms_main_loop_init() — config load, subsystem init
 *   5. bms_main_loop_run() — never returns
 */
#include "board_outputs.h"
#include "board_inputs.h"
#include "board_clock.h"
#include "board_uart.h"
#include "board_spi.h"
#include "board_i2c.h"
#include "board_adc.h"
#include "board_can.h"
#include "bms_main_loop.h"
#include "bms_hal.h"

/* Boot banner — sent over UART at startup to confirm firmware is alive. */
static const char k_boot_banner[] =
    "\r\n--- BMS v" \
    "0.1.0" \
    " HW_PROFILE=" \
    "0x0001" \
    " ---\r\n";

int main(void) {
    /* 1. Permission outputs safe FIRST — before clock or peripheral init.
     *    GPIO clock is on at reset (HSI); GPIOB is accessible immediately. */
    board_outputs_init_safe();

    /* 2. Clock and peripheral bus clocks */
    board_clock_init();

    /* 3. Peripheral init */
    board_inputs_init();
    board_uart_init();
    board_spi_init();
    board_i2c_init();
    board_adc_init();
    board_can_init();

    /* 4. Boot banner */
    board_uart_write((const uint8_t *)k_boot_banner, (uint16_t)(sizeof(k_boot_banner) - 1u));

    /* 5. BMS subsystem init + main loop */
    bms_main_loop_init();
    bms_main_loop_run();

    /* unreachable */
    return 0;
}
