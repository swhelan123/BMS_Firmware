/* board_inputs.h — digital input reads (charge detect, power button). */
#pragma once
#include <stdbool.h>

/* Configure PB4 (POWER_BUTTON) and PC14 (CHARGE_DETECT) as digital inputs.
 * Must be called after board_clock_init() (GPIO port clocks enabled). */
void board_inputs_init(void);

/* Raw charge-detect level. HIGH = charger present (per hardware contract:
 * CHARGE_DETECT rising edge is a wake source).
 * OQ-CD: confirm polarity against the adapted board before first charge. */
bool board_inputs_charge_detect(void);

/* Raw power-button level. */
bool board_inputs_power_button(void);
