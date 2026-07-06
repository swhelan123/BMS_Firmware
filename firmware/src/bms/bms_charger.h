/* bms_charger.h — Elcon/TC Charger CAN control (CC/CV charging).
 *
 * Protocol (extended 29-bit CAN IDs, 250 kbit/s, big-endian payload fields):
 *
 *   Command  BMS -> charger, ID 0x1806E5F4, sent at 1 Hz (charger stops
 *            itself if it goes 5 s without a valid command — a backstop,
 *            not the primary safety mechanism):
 *              byte 0-1: voltage setpoint, 0.1 V/bit
 *              byte 2-3: current setpoint, 0.1 A/bit
 *              byte 4:   control — 0x00 = charge, 0x01 = stop
 *              byte 5-7: reserved, 0
 *
 *   Status   charger -> BMS, ID 0x18FF50E5, ~1 Hz:
 *              byte 0-1: output voltage, 0.1 V/bit
 *              byte 2-3: output current, 0.1 A/bit
 *              byte 4:   status flags (bit=1 means active):
 *                          bit0 hardware failure
 *                          bit1 over-temperature
 *                          bit2 AC input out of range
 *                          bit3 battery not detected / reverse connection
 *                          bit4 communication timeout
 *              byte 5-7: reserved
 *
 * The charger does CC then CV and holds CV indefinitely once reached — it
 * never stops itself on taper. Termination is entirely the BMS's call: hold
 * the taper (output current below charge_taper_current_da) for
 * charge_taper_hold_ms, or any required cell reaching cell_ov_soft_mv, then
 * command stop (control=0x01) and drop charge permission. The hardware
 * cell_ov_hard_mv fault path is untouched and independent — it still opens
 * the shutdown circuit regardless of what this module or the CAN link does.
 *
 * Confirmed byte-for-byte against datasheets/Elcon-CAN-Specification.pdf
 * (Elcon doc 0010, rev 0A) — IDs, scaling, control polarity, and status bit
 * layout all match, including 0x00=start/0x01=stop. Still bench-verify with
 * the real charger before trusting it with a pack — a datasheet match is not
 * a substitute for observed hardware behaviour, just a much stronger prior.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bms_types.h"
#include "bms_config.h"

#define CHARGER_CAN_CMD_ID     (0x1806E5F4u)  /* BMS -> charger (extended) */
#define CHARGER_CAN_STATUS_ID  (0x18FF50E5u)  /* charger -> BMS (extended) */

#define CHARGER_CTRL_CHARGE    (0x00u)
#define CHARGER_CTRL_STOP      (0x01u)

#define CHARGER_CMD_PERIOD_MS  (1000u)  /* charger's own no-comms timeout is 5000 ms */

typedef struct {
    bool     status_valid;       /* a status frame has been received at all */
    uint16_t output_voltage_dv;
    uint16_t output_current_da;
    uint8_t  status_flags;       /* raw byte 4 — see bit meanings above */
    uint32_t last_status_ms;
} ChargerStatus;

typedef enum {
    CHARGER_STATUS_HW_FAILURE   = 0x01u,
    CHARGER_STATUS_OVER_TEMP    = 0x02u,
    CHARGER_STATUS_AC_INPUT     = 0x04u,
    CHARGER_STATUS_NO_BATTERY   = 0x08u,
    CHARGER_STATUS_COMM_TIMEOUT = 0x10u,
} ChargerStatusFlag;

/* ── Pure encode/decode — no hardware access, host-testable ──────────────── */

/* Build the 8-byte command frame. control must be CHARGER_CTRL_CHARGE or
 * CHARGER_CTRL_STOP. voltage_dv/current_da are NOT re-clamped here — the
 * caller (bms_charger_tick) is responsible for clamping against config. */
void bms_charger_build_command(uint8_t out[8], uint16_t voltage_dv,
                                uint16_t current_da, uint8_t control);

/* Parse an 8-byte status frame. Returns false if len < 5 (not enough bytes
 * for the fields this driver uses). */
bool bms_charger_parse_status(const uint8_t *data, uint8_t len, ChargerStatus *out);

/* True once output_current_da has stayed below cfg->charge_taper_current_da
 * for at least cfg->charge_taper_hold_ms, given the last status update and
 * how long the taper condition has now held. Pure decision function; the
 * hold-timer bookkeeping lives in the caller (bms_charger_tick). */
bool bms_charger_taper_complete(const BmsConfig *cfg, uint16_t output_current_da,
                                 uint32_t taper_held_ms);

/* ── Stateful control (hardware-touching, not unit tested) ───────────────── */

/* Enter charge-CAN mode: switches the CAN peripheral to 250 kbit/s and
 * starts the 1 Hz command heartbeat. Call once on STANDBY -> CHARGE. */
void bms_charger_start(void);

/* Leave charge-CAN mode: sends one stop command (best-effort), then
 * switches the CAN peripheral back to 500 kbit/s drive mode. Call once on
 * any CHARGE -> STANDBY transition (charger unplugged, fault, or the BMS's
 * own termination decision). */
void bms_charger_stop(void);

/* Call every main-loop iteration while BMS_STATE_CHARGE is active. Sends
 * the command heartbeat on schedule, drains any pending status frame, and
 * updates the termination decision. */
void bms_charger_tick(const BmsConfig *cfg);

/* True once bms_charger_tick() has decided the charge should end (voltage
 * or taper termination). bms_state uses this alongside charger-present and
 * fault checks to leave BMS_STATE_CHARGE. */
bool bms_charger_termination_requested(void);

/* Last received charger status (for diagnostics/telemetry). */
const ChargerStatus *bms_charger_get_status(void);
