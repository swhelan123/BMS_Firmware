/* board_outputs.c — permission output implementation.
 *
 * See board_outputs.h for the output polarity contract.
 * This file is the ONLY place in the firmware that drives permission GPIOs.
 */
#include "board_outputs.h"
#include "board_pins.h"
#include "bms_hal.h"
#include <stdint.h>

/* ── GPIO helpers ─────────────────────────────────────────────────────────── */

static inline void gpio_set_pin(GPIO_TypeDef *port, uint32_t pin) {
    port->BSRR = (uint32_t)(1u << pin);
}

static inline void gpio_clear_pin(GPIO_TypeDef *port, uint32_t pin) {
    port->BSRR = (uint32_t)(1u << (pin + 16u));
}

/* ── Output polarity encoding ─────────────────────────────────────────────── */
/* All four permission outputs (PB10/PB11/PB0/PB2) use the same MOSFET stage:
 *   MCU HIGH → MOSFET on → drain pulled LOW → downstream active-low signal asserted.
 *   MCU LOW  → MOSFET off → downstream pulled HIGH → signal inactive (safe default).
 * Logical true (permission active) maps to MCU HIGH for all four outputs.
 * PB5 (PowerEnable): MCU HIGH = power latch held. */
static inline void perm_output_set(GPIO_TypeDef *port, uint32_t pin, bool active) {
    if (active) {
        gpio_set_pin(port, pin);
    } else {
        gpio_clear_pin(port, pin);
    }
}

/* ── Shadow state (logical permission state, not MCU pin level) ──────────── */
static BmsOutputsBitmask s_state;

/* ── API ──────────────────────────────────────────────────────────────────── */

/* Configure one pin as a push-pull output (MODER = 01). */
static inline void gpio_make_output(GPIO_TypeDef *port, uint32_t pin) {
    port->MODER = (port->MODER & ~(3u << (pin * 2u)))
                  | (GPIO_MODER_OUTPUT << (pin * 2u));
}

void board_outputs_init_safe(void) {
    /* This runs before board_clock_init() (see main.c) so it must enable the
     * GPIOB peripheral clock itself — AHB GPIO clocks are gated OFF at reset
     * on STM32F3; BSRR/MODER writes are silently lost otherwise. */
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
    (void)RCC->AHBENR; /* read-back: ensure clock is up before register writes */

    /* Drive all permission outputs inactive (MCU LOW) FIRST, so the pins
     * present the safe level the instant MODER switches them to output. */
    gpio_clear_pin(OUTPUT_PORT_B, PIN_MASTER_OK);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_DISCHARGE_ENABLE);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_CHARGE_ENABLE);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_CHARGER_SAFETY);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_LED0);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_POWER_LED);
    /* PowerEnable is asserted here to keep the board alive after init.
     * It is only deasserted as the last step in the shutdown sequence. */
    gpio_set_pin(OUTPUT_PORT_B, PIN_POWER_ENABLE);

    /* Now switch the pins to output mode. Note PB3 (POWER_LED) defaults to
     * JTDO (AF mode) at reset and must be reclaimed explicitly. */
    gpio_make_output(OUTPUT_PORT_B, PIN_MASTER_OK);
    gpio_make_output(OUTPUT_PORT_B, PIN_DISCHARGE_ENABLE);
    gpio_make_output(OUTPUT_PORT_B, PIN_CHARGE_ENABLE);
    gpio_make_output(OUTPUT_PORT_B, PIN_CHARGER_SAFETY);
    gpio_make_output(OUTPUT_PORT_B, PIN_LED0);
    gpio_make_output(OUTPUT_PORT_B, PIN_POWER_LED);
    gpio_make_output(OUTPUT_PORT_B, PIN_POWER_ENABLE);

    s_state = 0;
}

void board_outputs_set_master_ok(bool allowed) {
    perm_output_set(OUTPUT_PORT_B, PIN_MASTER_OK, allowed);
    if (allowed) {
        s_state |= OUTPUTS_BIT_MASTER_OK;
    } else {
        s_state &= (BmsOutputsBitmask)(~OUTPUTS_BIT_MASTER_OK);
    }
}

void board_outputs_set_discharge_permission(bool allowed) {
    perm_output_set(OUTPUT_PORT_B, PIN_DISCHARGE_ENABLE, allowed);
    if (allowed) {
        s_state |= OUTPUTS_BIT_DISCHARGE;
    } else {
        s_state &= (BmsOutputsBitmask)(~OUTPUTS_BIT_DISCHARGE);
    }
}

void board_outputs_set_charge_permission(bool allowed) {
    perm_output_set(OUTPUT_PORT_B, PIN_CHARGE_ENABLE, allowed);
    if (allowed) {
        s_state |= OUTPUTS_BIT_CHARGE;
    } else {
        s_state &= (BmsOutputsBitmask)(~OUTPUTS_BIT_CHARGE);
    }
}

void board_outputs_set_charger_safety(bool allowed) {
    perm_output_set(OUTPUT_PORT_B, PIN_CHARGER_SAFETY, allowed);
    if (allowed) {
        s_state |= OUTPUTS_BIT_CHARGER_SAFETY;
    } else {
        s_state &= (BmsOutputsBitmask)(~OUTPUTS_BIT_CHARGER_SAFETY);
    }
}

void board_outputs_assert_power_enable(void) {
    gpio_set_pin(OUTPUT_PORT_B, PIN_POWER_ENABLE);
}

void board_outputs_release_power(void) {
    /* Releasing power latch causes board to lose power.
     * All permissions must already be deasserted before calling this. */
    gpio_clear_pin(OUTPUT_PORT_B, PIN_POWER_ENABLE);
}

void board_outputs_set_led0(bool on) {
    if (on) { gpio_set_pin(OUTPUT_PORT_B, PIN_LED0); }
    else     { gpio_clear_pin(OUTPUT_PORT_B, PIN_LED0); }
}

void board_outputs_set_power_led(bool on) {
    if (on) { gpio_set_pin(OUTPUT_PORT_B, PIN_POWER_LED); }
    else    { gpio_clear_pin(OUTPUT_PORT_B, PIN_POWER_LED); }
}

void board_outputs_disable_all(void) {
    gpio_clear_pin(OUTPUT_PORT_B, PIN_MASTER_OK);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_DISCHARGE_ENABLE);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_CHARGE_ENABLE);
    gpio_clear_pin(OUTPUT_PORT_B, PIN_CHARGER_SAFETY);
    s_state = 0;
}

BmsOutputsBitmask board_outputs_get_state(void) {
    return s_state;
}

void board_outputs_get_gpio_snapshot(BmsGpioSnapshot *out) {
    /* Read raw IDR bits — no writes, no side effects. */
    uint32_t idr_a = GPIOA->IDR;
    uint32_t idr_b = GPIOB->IDR;
    uint32_t idr_c = GPIOC->IDR;
    out->cs_cell            = (uint8_t)((idr_a >> CS_CELL_PIN)         & 1u);
    out->cs_temp            = (uint8_t)((idr_b >> CS_TEMP_PIN)         & 1u);
    out->power_button       = (uint8_t)((idr_b >> PIN_POWER_BUTTON)    & 1u);
    out->charge_detect      = (uint8_t)((idr_c >> PIN_CHARGE_DETECT)   & 1u);
    out->power_enable       = (uint8_t)((idr_b >> PIN_POWER_ENABLE)    & 1u);
    out->master_ok_raw      = (uint8_t)((idr_b >> PIN_MASTER_OK)       & 1u);
    out->discharge_raw      = (uint8_t)((idr_b >> PIN_DISCHARGE_ENABLE)& 1u);
    out->charge_raw         = (uint8_t)((idr_b >> PIN_CHARGE_ENABLE)   & 1u);
    out->charger_safety_raw = (uint8_t)((idr_b >> PIN_CHARGER_SAFETY)  & 1u);
}
