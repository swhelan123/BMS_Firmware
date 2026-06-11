/* bms_faults.c — fault evaluation, latching, and clearing.
 *
 * Fault bits match protocol/fault_bits.yaml exactly.
 * Bit positions must never be changed without updating the YAML and Python tool.
 */
#include "bms_faults.h"
#include "bms_config.h"
#include "board_clock.h"

static uint64_t s_active;
static uint64_t s_latched;

/* PEC consecutive error counters for ISOSPI fault escalation */
static uint8_t s_pec_consec_cell;
static uint8_t s_pec_consec_temp;

static inline void set_active(FaultBit bit, bool cond) {
    if (cond) {
        s_active  |= FAULT_MASK(bit);
        /* Only faults marked latching:true in fault_bits.yaml stick in the
         * latched word; everything else tracks its active condition only. */
        s_latched |= FAULT_MASK(bit) & FAULT_LATCHING_MASK;
    } else {
        s_active &= ~FAULT_MASK(bit);
    }
}

void bms_faults_evaluate(const CellSnapshot    *cells,
                          const TempSnapshot    *temps,
                          const PackMeasurement *pack,
                          const BmsConfig       *cfg) {
    uint32_t now = board_clock_get_ms();

    /* ── Cell measurement validity / staleness ────────────────────────────── */
    bool cells_stale = (cells->overall == MEAS_ERROR) ||
                       ((now - cells->timestamp_ms) > cfg->stale_data_timeout_ms);
    bool temps_stale = (temps->overall == MEAS_ERROR) ||
                       ((now - temps->timestamp_ms) > cfg->stale_data_timeout_ms);
    bool pack_stale  = (now - pack->timestamp_ms) > cfg->stale_data_timeout_ms;

    set_active(FAULT_BIT_CELL_READ_INVALID, cells_stale);
    set_active(FAULT_BIT_TEMP_READ_INVALID, temps_stale);
    set_active(FAULT_BIT_VBAT_INVALID,  pack_stale || !pack->vbat_valid);
    set_active(FAULT_BIT_VPACK_INVALID, pack_stale || !pack->vpack_valid);

    /* ── isoSPI fault escalation (fed by bms_faults_report_pec_error) ──────── */
    /* Evaluated externally via bms_faults_report_pec_error() */

    /* ── Cell voltage faults ──────────────────────────────────────────────── */
    bool any_uv_hard = false, any_uv_soft = false;
    bool any_ov_hard = false, any_ov_soft = false;
    if (!cells_stale) {
        for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
            if (!cells->valid[i]) { continue; }
            uint8_t byte = i / 8u;
            uint8_t bit  = i % 8u;
            if (!(cfg->required_cell_mask[byte] & (1u << bit))) { continue; }
            uint16_t mv = cells->mv[i];
            if (mv < cfg->cell_uv_hard_mv) { any_uv_hard = true; }
            if (mv < cfg->cell_uv_soft_mv) { any_uv_soft = true; }
            if (mv > cfg->cell_ov_hard_mv) { any_ov_hard = true; }
            if (mv > cfg->cell_ov_soft_mv) { any_ov_soft = true; }
        }
    }
    set_active(FAULT_BIT_CELL_OV,      any_ov_hard);
    set_active(FAULT_BIT_CELL_OV_SOFT, any_ov_soft);
    set_active(FAULT_BIT_CELL_UV,      any_uv_hard);
    set_active(FAULT_BIT_CELL_UV_SOFT, any_uv_soft);

    /* ── Temperature faults ───────────────────────────────────────────────── */
    bool temp_over_charge    = false;
    bool temp_over_discharge = false;
    bool temp_over_abs       = false;
    bool temp_cold_charge    = false;
    bool temp_cold_discharge = false;
    bool temp_coverage_ok    = true;  /* assume valid until proven otherwise */

    if (!temps_stale) {
        for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
            uint8_t byte = i / 8u;
            uint8_t tbit = i % 8u;
            bool required = (cfg->required_temp_mask[byte] & (1u << tbit)) != 0u;
            if (!required) { continue; }
            if (!temps->valid[i] || temps->cx10[i] == TEMP_INVALID_CX10) {
                temp_coverage_ok = false;
                continue;
            }
            int16_t t = temps->cx10[i];
            if (t >= cfg->temp_charge_hard_cx10)    { temp_over_charge = true; }
            if (t >= cfg->temp_discharge_hard_cx10) { temp_over_discharge = true; }
            if (t >= cfg->temp_hard_abs_cx10)        { temp_over_abs = true; }
            if (t < cfg->temp_cold_charge_limit_cx10)    { temp_cold_charge = true; }
            if (t < cfg->temp_cold_discharge_limit_cx10) { temp_cold_discharge = true; }
        }
    } else {
        temp_coverage_ok = false;
    }

    set_active(FAULT_BIT_TEMP_OVER_CHARGE,   temp_over_charge);
    set_active(FAULT_BIT_TEMP_OVER_DISCHARGE, temp_over_discharge);
    set_active(FAULT_BIT_TEMP_OVER_ABS,      temp_over_abs);
    set_active(FAULT_BIT_TEMP_COLD_CHARGE,   temp_cold_charge);
    set_active(FAULT_BIT_TEMP_COLD_DISCHARGE, temp_cold_discharge);
    set_active(FAULT_BIT_TEMP_COVERAGE,      !temp_coverage_ok);

    /* ── Current faults ───────────────────────────────────────────────────── */
    if (pack->i_batt_valid) {
        uint32_t abs_i = (pack->i_batt_ma >= 0) ? (uint32_t)pack->i_batt_ma
                                                  : (uint32_t)(-pack->i_batt_ma);
        set_active(FAULT_BIT_OVERCURRENT, abs_i > cfg->overcurrent_hard_ma);
    } else {
        set_active(FAULT_BIT_OVERCURRENT, false);
    }

    /* ── I2C fault ────────────────────────────────────────────────────────── */
    /* Fed externally by bms_faults_report_i2c_error() */
}

void bms_faults_report_pec_error(BmsChain chain) {
    if (chain == BMS_CHAIN_CELL) {
        if (s_pec_consec_cell < LTC6812_MAX_RETRIES) {
            s_pec_consec_cell++;
        }
        if (s_pec_consec_cell >= LTC6812_MAX_RETRIES) {
            set_active(FAULT_BIT_ISOSPI_CELL, true);
        }
    } else {
        if (s_pec_consec_temp < LTC6812_MAX_RETRIES) {
            s_pec_consec_temp++;
        }
        if (s_pec_consec_temp >= LTC6812_MAX_RETRIES) {
            set_active(FAULT_BIT_ISOSPI_TEMP, true);
        }
    }
}

void bms_faults_clear_pec_counter(BmsChain chain) {
    if (chain == BMS_CHAIN_CELL) {
        s_pec_consec_cell = 0;
        s_active &= ~FAULT_MASK(FAULT_BIT_ISOSPI_CELL);
    } else {
        s_pec_consec_temp = 0;
        s_active &= ~FAULT_MASK(FAULT_BIT_ISOSPI_TEMP);
    }
}

void bms_faults_report_i2c_error(void) {
    set_active(FAULT_BIT_I2C_ISL28022, true);
}

void bms_faults_clear_i2c_error(void) {
    s_active &= ~FAULT_MASK(FAULT_BIT_I2C_ISL28022);
}

uint64_t bms_faults_get_active(void)  { return s_active;  }
uint64_t bms_faults_get_latched(void) { return s_latched; }

uint64_t bms_faults_clear_latched(uint64_t mask) {
    uint64_t clearable = mask & s_latched & ~s_active;
    s_latched &= ~clearable;
    return clearable;
}

void bms_faults_set(FaultBit bit) {
    set_active(bit, true);
}

void bms_faults_set_latched(FaultBit bit) {
    /* Latch a fault without marking it active. Used for conditions detected
     * after the fact (e.g. FAULT_BIT_WATCHDOG when an IWDG-caused reset is
     * found at boot): the condition itself is historical, but the fault must
     * block permissions until explicitly acknowledged and cleared. */
    s_latched |= FAULT_MASK(bit);
}

void bms_faults_apply_openwire(const bool detected[TOTAL_CELL_COUNT],
                                const BmsConfig *cfg) {
    bool any_open = false;
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        if (!(cfg->required_cell_mask[i / 8u] & (1u << (i % 8u)))) { continue; }
        if (detected[i]) { any_open = true; break; }
    }
    /* Latching per fault_bits.yaml: active clears on a clean scan, latched
     * persists until explicitly cleared. */
    set_active(FAULT_BIT_CELL_OPENWIRE, any_open);
}
