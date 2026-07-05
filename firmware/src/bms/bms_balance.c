/* bms_balance.c — cell balance logic.
 *
 * Algorithm: discharge any cell that is more than cell_balance_hysteresis_mv
 * above the pack minimum AND above cell_balance_target_mv. Only cells in
 * balance_allowed_mask are eligible. Balancing is duty-cycled using
 * balance_on_time_ms / balance_off_time_ms to bound resistor heating.
 *
 * Safety: never called with TEMP chain; blocked by FAULT_BLOCKS_BALANCING_MASK.
 */
#include "bms_balance.h"
#include "ltc6812.h"
#include "board_clock.h"
#include <string.h>

static bool     s_is_on_phase;
static uint32_t s_phase_start_ms;
static bool     s_phase_active;   /* true once first balance-eligible cycle seen */

static inline bool cell_in_mask(const uint8_t mask[CONFIG_MASK_BYTES], uint8_t idx) {
    return (bool)(mask[idx / 8u] & (1u << (idx % 8u)));
}

void bms_balance_tick(const CellSnapshot *cells,
                       uint64_t            active_faults,
                       BmsState            state,
                       const BmsConfig    *cfg) {
    bool should_balance = (state == BMS_STATE_STANDBY || state == BMS_STATE_DISCHARGE)
                          && !(active_faults & FAULT_BLOCKS_BALANCING_MASK)
                          && (cells->overall != MEAS_ERROR);

    if (!should_balance) {
        bms_balance_disable_all();
        s_phase_active = false;
        return;
    }

    /* Find minimum valid voltage across required cells. */
    uint16_t min_mv = UINT16_MAX;
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        if (!cell_in_mask(cfg->required_cell_mask, i)) { continue; }
        if (!cells->valid[i]) { continue; }
        if (cells->mv[i] < min_mv) { min_mv = cells->mv[i]; }
    }

    if (min_mv == UINT16_MAX) {
        /* No valid required cells — cannot balance safely. */
        bms_balance_disable_all();
        s_phase_active = false;
        return;
    }

    /* Build per-IC DCC mask: bit N of dcc_mask[ic] = balance cell (ic*15 + N + 1). */
    uint16_t dcc_mask[CELL_IC_COUNT];
    memset(dcc_mask, 0, sizeof(dcc_mask));
    bool any_to_balance = false;

    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        if (!cell_in_mask(cfg->balance_allowed_mask, i)) { continue; }
        if (!cells->valid[i]) { continue; }
        uint16_t mv = cells->mv[i];
        if (mv > cfg->cell_balance_target_mv &&
            mv > (uint16_t)(min_mv + cfg->cell_balance_hysteresis_mv)) {
            uint8_t ic  = i / CELLS_PER_IC;
            uint8_t bit = i % CELLS_PER_IC;
            dcc_mask[ic] |= (uint16_t)(1u << bit);
            any_to_balance = true;
        }
    }

    if (!any_to_balance) {
        bms_balance_disable_all();
        s_phase_active = false;
        return;
    }

    /* Duty-cycle management. */
    uint32_t now = board_clock_get_ms();
    if (!s_phase_active) {
        s_is_on_phase    = true;
        s_phase_start_ms = now;
        s_phase_active   = true;
    } else {
        uint32_t elapsed = now - s_phase_start_ms;
        if (s_is_on_phase && elapsed >= cfg->balance_on_time_ms) {
            s_is_on_phase    = false;
            s_phase_start_ms = now;
        } else if (!s_is_on_phase && elapsed >= cfg->balance_off_time_ms) {
            s_is_on_phase    = true;
            s_phase_start_ms = now;
        }
    }

    const uint8_t active_ics = bms_config_active_cell_ics();
    if (s_is_on_phase) {
        ltc6812_cell_chain_set_balance(BMS_CHAIN_CELL, active_ics, dcc_mask);
    } else {
        ltc6812_cell_chain_clear_balance(BMS_CHAIN_CELL, active_ics);
    }
}

void bms_balance_disable_all(void) {
    ltc6812_cell_chain_clear_balance(BMS_CHAIN_CELL, bms_config_active_cell_ics());
}
