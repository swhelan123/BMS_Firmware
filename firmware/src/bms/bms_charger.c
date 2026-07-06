/* bms_charger.c — Elcon/TC Charger CAN control. See bms_charger.h. */
#include "bms_charger.h"
#include "bms_measurements.h"
#include "board_can.h"
#include "board_clock.h"
#include <string.h>

/* ── Pure encode/decode ───────────────────────────────────────────────────── */

void bms_charger_build_command(uint8_t out[8], uint16_t voltage_dv,
                                uint16_t current_da, uint8_t control) {
    out[0] = (uint8_t)(voltage_dv >> 8u);
    out[1] = (uint8_t)(voltage_dv & 0xFFu);
    out[2] = (uint8_t)(current_da >> 8u);
    out[3] = (uint8_t)(current_da & 0xFFu);
    out[4] = control;
    out[5] = 0u;
    out[6] = 0u;
    out[7] = 0u;
}

bool bms_charger_parse_status(const uint8_t *data, uint8_t len, ChargerStatus *out) {
    if (len < 5u) { return false; }
    out->output_voltage_dv = (uint16_t)((uint16_t)data[0] << 8u) | data[1];
    out->output_current_da = (uint16_t)((uint16_t)data[2] << 8u) | data[3];
    out->status_flags      = data[4];
    out->status_valid      = true;
    out->last_status_ms    = board_clock_get_ms();
    return true;
}

bool bms_charger_taper_complete(const BmsConfig *cfg, uint16_t output_current_da,
                                 uint32_t taper_held_ms) {
    if (output_current_da >= cfg->charge_taper_current_da) { return false; }
    return taper_held_ms >= cfg->charge_taper_hold_ms;
}

/* ── Stateful control ─────────────────────────────────────────────────────── */

static ChargerStatus s_status;
static uint32_t      s_last_cmd_ms;
static uint32_t       s_taper_start_ms;
static bool           s_taper_active;
static bool           s_termination_requested;

void bms_charger_start(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_taper_active           = false;
    s_termination_requested  = false;
    board_can_set_charge_mode(true);
    s_last_cmd_ms = board_clock_get_ms() - CHARGER_CMD_PERIOD_MS; /* send immediately */
}

void bms_charger_stop(void) {
    uint8_t frame[8];
    bms_charger_build_command(frame, 0u, 0u, CHARGER_CTRL_STOP);
    board_can_send_ext(CHARGER_CAN_CMD_ID, frame, 8u); /* best-effort */
    board_can_set_charge_mode(false);
}

/* Any required cell at/above cell_ov_soft_mv — the same threshold the
 * discharge-side fault path uses as its early-warning limit before hard OV. */
static bool any_required_cell_at_soft_ov(const BmsConfig *cfg) {
    const CellSnapshot *cells = bms_measurements_get_cells();
    for (uint8_t i = 0u; i < cfg->cell_count; i++) {
        bool required = (cfg->required_cell_mask[i / 8u] & (1u << (i % 8u))) != 0u;
        if (!required || !cells->valid[i]) { continue; }
        if (cells->mv[i] >= cfg->cell_ov_soft_mv) { return true; }
    }
    return false;
}

void bms_charger_tick(const BmsConfig *cfg) {
    uint32_t now = board_clock_get_ms();

    /* Drain any pending status frame(s) this tick. */
    uint32_t id; bool ext; uint8_t data[8]; uint8_t len;
    while (board_can_receive(&id, &ext, data, &len)) {
        if (ext && id == CHARGER_CAN_STATUS_ID) {
            bms_charger_parse_status(data, len, &s_status);
        }
    }

    /* Termination check: voltage first (cheaper, and the more urgent of the
     * two), then taper hold. Once requested, latch it — bms_charger_stop()
     * will be called on the resulting CHARGE->STANDBY transition. */
    if (!s_termination_requested) {
        if (any_required_cell_at_soft_ov(cfg)) {
            s_termination_requested = true;
        } else if (s_status.status_valid) {
            if (s_status.output_current_da < cfg->charge_taper_current_da) {
                if (!s_taper_active) {
                    s_taper_active   = true;
                    s_taper_start_ms = now;
                }
                if (bms_charger_taper_complete(cfg, s_status.output_current_da,
                                               now - s_taper_start_ms)) {
                    s_termination_requested = true;
                }
            } else {
                s_taper_active = false;
            }
        }
    }

    /* Command heartbeat: clamp setpoints against config every time, not just
     * at start, in case config changed mid-charge (shouldn't happen in
     * practice, but never trust a stale clamp). */
    if ((now - s_last_cmd_ms) >= CHARGER_CMD_PERIOD_MS) {
        s_last_cmd_ms = now;
        uint16_t voltage_dv = cfg->charge_voltage_setpoint_dv;
        if (voltage_dv > cfg->charge_voltage_max_dv) { voltage_dv = cfg->charge_voltage_max_dv; }
        uint8_t control = s_termination_requested ? CHARGER_CTRL_STOP : CHARGER_CTRL_CHARGE;
        uint8_t frame[8];
        bms_charger_build_command(frame, voltage_dv, cfg->charge_current_setpoint_da, control);
        board_can_send_ext(CHARGER_CAN_CMD_ID, frame, 8u);
    }
}

bool bms_charger_termination_requested(void) {
    return s_termination_requested;
}

const ChargerStatus *bms_charger_get_status(void) {
    return &s_status;
}
