/* board_outputs.h — permission output API.
 *
 * This is the ONLY module that calls the MCU GPIO set functions for permission
 * outputs. All other modules call board_outputs_set_*() via this API.
 *
 * Output semantics (STM32F303VC MCU-level, confirmed from schematic):
 *   All four permission outputs (PB10/PB11/PB0/PB2) share the same MOSFET model:
 *     MCU HIGH → MOSFET on → downstream LOW → permission ACTIVE.
 *     MCU LOW  → MOSFET off → downstream HIGH → permission INACTIVE (safe).
 *   PB5 (PowerEnable): HIGH keeps board alive; LOW releases power latch.
 *   PB1 (LED0), PB3 (PowerLED): HIGH = LED on.
 *
 * Safe default: all permission outputs at MCU LOW (downstream inactive).
 */
#pragma once
#include <stdbool.h>
#include "bms_types.h"

/* Initialise permission GPIOs to safe (inactive) state. Must be the first
 * board function called on any power-up or reset path. */
void board_outputs_init_safe(void);

/* Individual permission setters. True = permission active; false = inactive.
 * Each function encodes the MCU-level polarity for the downstream circuit. */
void board_outputs_set_master_ok(bool allowed);
void board_outputs_set_discharge_permission(bool allowed);
void board_outputs_set_charge_permission(bool allowed);
void board_outputs_set_charger_safety(bool allowed);

/* Power latch — call board_outputs_release_power() only after all permission
 * outputs have been deasserted and the shutdown sequence is complete. */
void board_outputs_assert_power_enable(void);
void board_outputs_release_power(void);

/* Status LEDs */
void board_outputs_set_led0(bool on);
void board_outputs_set_power_led(bool on);

/* Emergency safe-state: deasserts all permission outputs in one call.
 * Called from fatal fault paths and the IWDG reset vector. */
void board_outputs_disable_all(void);

/* Read-back current logical output state (not MCU pin level). */
BmsOutputsBitmask board_outputs_get_state(void);

/* ── Read-only GPIO snapshot for bring-up diagnostics ─────────────────────── */
/* Reads raw MCU input data registers — no writes, no side effects.
 * All fields are raw MCU pin levels (0 or 1). */
typedef struct {
    uint8_t cs_cell;          /* PA4  — CS CELL chain (idle=1, active=0) */
    uint8_t cs_temp;          /* PB12 — CS TEMP chain (idle=1, active=0) */
    uint8_t power_button;     /* PB4  — Power button input */
    uint8_t charge_detect;    /* PC14 — Charger present input */
    uint8_t power_enable;     /* PB5  — Power latch raw MCU level */
    uint8_t master_ok_raw;    /* PB11 — MasterOk raw MCU level */
    uint8_t discharge_raw;    /* PB10 — Discharge_enable raw MCU level */
    uint8_t charge_raw;       /* PB0  — Charge permission raw MCU level */
    uint8_t charger_safety_raw; /* PB2 — ChargerSafety raw MCU level */
} BmsGpioSnapshot;

void board_outputs_get_gpio_snapshot(BmsGpioSnapshot *out);
