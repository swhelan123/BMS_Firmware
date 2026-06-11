/* board_inputs.c — digital input configuration and reads.
 *
 * PB4  POWER_BUTTON  — defaults to NJTRST (AF mode) at reset; must be
 *                      explicitly reclaimed as a GPIO input.
 * PC14 CHARGE_DETECT — defaults to input; set explicitly anyway so the
 *                      configuration does not depend on reset state.
 */
#include "board_inputs.h"
#include "board_pins.h"
#include "bms_hal.h"

void board_inputs_init(void) {
    /* GPIOB and GPIOC clocks are enabled by board_clock_init(); enabling
     * them again here is harmless and makes this function order-tolerant. */
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN | RCC_AHBENR_GPIOCEN;
    (void)RCC->AHBENR;

    /* PB4 → input (reclaims NJTRST). No pull: board provides the divider. */
    GPIOB->MODER &= ~(3u << (PIN_POWER_BUTTON * 2u));
    GPIOB->PUPDR &= ~(3u << (PIN_POWER_BUTTON * 2u));

    /* PC14 → input. No pull: charger-present signal is externally driven. */
    CHARGE_DETECT_PORT->MODER &= ~(3u << (PIN_CHARGE_DETECT * 2u));
    CHARGE_DETECT_PORT->PUPDR &= ~(3u << (PIN_CHARGE_DETECT * 2u));
}

bool board_inputs_charge_detect(void) {
    return (CHARGE_DETECT_PORT->IDR >> PIN_CHARGE_DETECT) & 1u;
}

bool board_inputs_power_button(void) {
    return (GPIOB->IDR >> PIN_POWER_BUTTON) & 1u;
}
