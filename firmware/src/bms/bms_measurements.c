/* bms_measurements.c — periodic measurement acquisition.
 *
 * Temperature measurement uses ADCV on the TEMP chain (C-inputs), not ADAX.
 * Per hardware contract §7: sensors are measured on C-inputs via S-output bias.
 *
 * Enepaq V-T table: Sony-Murata NTC Table 5, 33 breakpoints -40°C to +120°C.
 * Voltages in mV (LTC6812 raw × 100µV / 10), temperatures in °C×10.
 */
#include "bms_measurements.h"
#include "bms_config.h"
#include "bms_faults.h"
#include "bms_diagnostics.h"
#include "ltc6812.h"
#include "isl28022.h"
#include "board_adc.h"
#include "board_clock.h"
#include "board_pins.h"
#include <string.h>
#include <limits.h>

static CellSnapshot    s_cells;
static TempSnapshot    s_temps;
static PackMeasurement s_pack;

/* ── Enepaq V-T lookup table ──────────────────────────────────────────────── */
/* Source: Sony-Murata NTC sensor Table 5 (confirmed datasheet values).
 * Sorted mv DESCENDING (highest voltage = lowest temperature).
 * Units: voltage in mV, temperature in °C×10.
 * Range: 2440 mV (−40°C) → 1300 mV (+120°C). Out-of-range → TEMP_INVALID_CX10. */
#define ENEPAQ_TABLE_POPULATED  1

typedef struct { uint16_t mv; int16_t cx10; } VTPoint;
static const VTPoint k_enepaq_vt[] = {
    /* { voltage_mv, temperature_cx10 } */
    { 2440, -400 },  /* -40°C */
    { 2420, -350 },  /* -35°C */
    { 2400, -300 },  /* -30°C */
    { 2380, -250 },  /* -25°C */
    { 2350, -200 },  /* -20°C */
    { 2320, -150 },  /* -15°C */
    { 2270, -100 },  /* -10°C */
    { 2230,  -50 },  /*  -5°C */
    { 2170,    0 },  /*   0°C */
    { 2110,   50 },  /*  +5°C */
    { 2050,  100 },  /* +10°C */
    { 1990,  150 },  /* +15°C */
    { 1920,  200 },  /* +20°C */
    { 1860,  250 },  /* +25°C */
    { 1800,  300 },  /* +30°C */
    { 1740,  350 },  /* +35°C */
    { 1680,  400 },  /* +40°C */
    { 1630,  450 },  /* +45°C */
    { 1590,  500 },  /* +50°C */
    { 1550,  550 },  /* +55°C */
    { 1510,  600 },  /* +60°C */
    { 1480,  650 },  /* +65°C */
    { 1450,  700 },  /* +70°C */
    { 1430,  750 },  /* +75°C */
    { 1400,  800 },  /* +80°C */
    { 1380,  850 },  /* +85°C */
    { 1370,  900 },  /* +90°C */
    { 1350,  950 },  /* +95°C */
    { 1340, 1000 },  /* +100°C */
    { 1330, 1050 },  /* +105°C */
    { 1320, 1100 },  /* +110°C */
    { 1310, 1150 },  /* +115°C */
    { 1300, 1200 },  /* +120°C */
};
#define ENEPAQ_VT_COUNT  (sizeof(k_enepaq_vt) / sizeof(k_enepaq_vt[0]))

/* Convert a measured voltage (in mV, 100µV LSB from LTC6812 raw/10) to °C×10.
 * Returns TEMP_INVALID_CX10 if voltage is outside table range [1300, 2440] mV. */
static int16_t enepaq_voltage_to_cx10(uint16_t mv) {
    if (mv > k_enepaq_vt[0].mv || mv < k_enepaq_vt[ENEPAQ_VT_COUNT - 1u].mv) {
        return TEMP_INVALID_CX10;
    }
    for (uint8_t i = 0u; i < ENEPAQ_VT_COUNT - 1u; i++) {
        if (mv <= k_enepaq_vt[i].mv && mv >= k_enepaq_vt[i + 1u].mv) {
            int32_t dv = (int32_t)k_enepaq_vt[i].mv - (int32_t)k_enepaq_vt[i + 1u].mv;
            int32_t dt = (int32_t)k_enepaq_vt[i + 1u].cx10 - (int32_t)k_enepaq_vt[i].cx10;
            int32_t offset = (int32_t)k_enepaq_vt[i].mv - (int32_t)mv;
            return (int16_t)((int32_t)k_enepaq_vt[i].cx10 + (offset * dt) / dv);
        }
    }
    return TEMP_INVALID_CX10;
}

/* ── Cell cycle ───────────────────────────────────────────────────────────── */
BmsResult bms_measurements_run_cell_cycle(void) {
    uint16_t raw_mv[CELL_IC_COUNT][CELLS_PER_IC];
    bool pec_ok[CELL_IC_COUNT];

    BmsResult r = ltc6812_read_cells(BMS_CHAIN_CELL, CELL_IC_COUNT, raw_mv, pec_ok);

    s_cells.timestamp_ms = board_clock_get_ms();

    if (r == BMS_OK) {
        for (uint8_t ic = 0; ic < CELL_IC_COUNT; ic++) {
            for (uint8_t c = 0; c < CELLS_PER_IC; c++) {
                uint8_t idx = ic * CELLS_PER_IC + c;
                s_cells.mv[idx]    = raw_mv[ic][c];
                s_cells.valid[idx] = pec_ok[ic];
            }
        }
        s_cells.overall = MEAS_VALID;
        bms_faults_clear_pec_counter(BMS_CHAIN_CELL);
    } else {
        s_cells.overall = MEAS_ERROR;
        for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) { s_cells.valid[i] = false; }
        if (r == BMS_ERR_PEC) { bms_faults_report_pec_error(BMS_CHAIN_CELL); }
    }

    return r;
}

/* ── Temperature cycle ────────────────────────────────────────────────────── */
/* Temperature sensors are on C-inputs of the TEMP chain. Measurement uses
 * ADCV + RDCVA-RDCVE (same as cell measurement), not ADAX + RDAUX.
 * S-outputs must be cleared on success AND every error path. */
BmsResult bms_measurements_run_temp_cycle(void) {
    const BmsConfig *cfg = bms_config_get();
    BmsResult r;

    /* 1. Assert S-outputs for all configured temp sensor channels.
     *    S-outputs are numbered per-IC; use full mask for all 15 channels. */
    r = ltc6812_temp_chain_set_sensor_bias(BMS_CHAIN_TEMP, TEMP_IC_COUNT, 0x7FFFu);
    if (r != BMS_OK) {
        ltc6812_temp_chain_clear_s_outputs(BMS_CHAIN_TEMP, TEMP_IC_COUNT);
        s_temps.overall = MEAS_ERROR;
        bms_faults_report_pec_error(BMS_CHAIN_TEMP);
        return r;
    }

    /* 2. Wait for sensor voltage to settle */
    board_clock_delay_ms(cfg->temp_settle_time_ms);

    /* 3. Run ADCV on TEMP chain and read RDCVA-RDCVE (C-input voltages) */
    uint16_t raw_mv[TEMP_IC_COUNT][CELLS_PER_IC];
    bool pec_ok[TEMP_IC_COUNT];
    r = ltc6812_read_cells(BMS_CHAIN_TEMP, TEMP_IC_COUNT, raw_mv, pec_ok);

    /* 4. ALWAYS clear S-outputs regardless of result */
    BmsResult clear_r = ltc6812_temp_chain_clear_s_outputs(BMS_CHAIN_TEMP, TEMP_IC_COUNT);

    s_temps.timestamp_ms = board_clock_get_ms();

    if (r == BMS_OK) {
        /* 5. Convert each channel voltage to temperature */
        for (uint8_t ic = 0; ic < TEMP_IC_COUNT; ic++) {
            for (uint8_t ch = 0; ch < TEMPS_PER_IC; ch++) {
                uint8_t idx = ic * TEMPS_PER_IC + ch;
                if (pec_ok[ic]) {
                    int16_t cx10 = enepaq_voltage_to_cx10(raw_mv[ic][ch]);
                    s_temps.cx10[idx]  = cx10;
                    s_temps.valid[idx] = (cx10 != TEMP_INVALID_CX10);
                } else {
                    s_temps.cx10[idx]  = TEMP_INVALID_CX10;
                    s_temps.valid[idx] = false;
                }
            }
        }
        /* Overall validity: valid only if all required sensors converted */
        s_temps.overall = MEAS_VALID; /* caller (bms_faults) checks coverage */
        bms_faults_clear_pec_counter(BMS_CHAIN_TEMP);
    } else {
        s_temps.overall = MEAS_ERROR;
        for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
            s_temps.cx10[i]  = TEMP_INVALID_CX10;
            s_temps.valid[i] = false;
        }
        if (r == BMS_ERR_PEC) { bms_faults_report_pec_error(BMS_CHAIN_TEMP); }
    }

    return (r != BMS_OK) ? r : clear_r;
}

/* ── Pack cycle (Vbat+I via ISL28022, Vpack via ADC1/PA1) ────────────────── */
BmsResult bms_measurements_run_pack_cycle(void) {
    const BmsConfig *cfg = bms_config_get();

    /* --- ISL28022: Vbat and I_batt --- */
    int32_t vbus_raw_mv = INT32_MIN;
    int32_t vshunt_raw_uv = 0;
    bool vbat_valid  = false;
    bool ibatt_valid = false;

    BmsResult r_isl = isl28022_read(&vbus_raw_mv, &vshunt_raw_uv);
    if (r_isl == BMS_OK) {
        vbat_valid  = true;
        ibatt_valid = true;
        bms_faults_clear_i2c_error();
    } else {
        bms_faults_report_i2c_error();
        bms_diagnostics_record_i2c_error();
    }

    /* Apply config-provided calibration: vbat_mv = raw * gain/1000 + offset */
    int32_t vbat_mv = INT32_MIN;
    if (vbat_valid) {
        vbat_mv = (vbus_raw_mv * (int32_t)cfg->vbat_gain_x1000) / 1000 +
                  (int32_t)cfg->vbat_offset_mv;
    }

    /* Current: vshunt_raw_uv / shunt_mohm → mA, then apply gain/offset.
     * current_gain_x1000 encodes (1000 / shunt_resistance_mohm) combined with
     * any additional calibration factor. i_batt_ma = raw_uv * gain / 1000000 + offset. */
    int32_t i_batt_ma = 0;
    if (ibatt_valid) {
        i_batt_ma = (vshunt_raw_uv * (int32_t)cfg->current_gain_x1000) / 1000000 +
                    (int32_t)cfg->current_offset_ma;
    }

    /* --- ADC1: Vpack on PA1 (ADC1_IN2) --- */
    uint16_t adc_raw = 0;
    bool vpack_valid = false;
    int32_t vpack_mv = INT32_MIN;

    BmsResult r_adc = board_adc_read_raw(&adc_raw);
    if (r_adc == BMS_OK) {
        vpack_valid = true;
        /* Convert 12-bit raw to mV using VREF, then apply gain/offset. */
        int32_t raw_mv = ((int32_t)adc_raw * (int32_t)VPACK_VREF_MV) / 4096;
        vpack_mv = (raw_mv * (int32_t)cfg->vpack_gain_x1000) / 1000 +
                   (int32_t)cfg->vpack_offset_mv;
    }

    bms_measurements_update_pack(vbat_mv, vpack_mv, i_batt_ma,
                                  vbat_valid, vpack_valid, ibatt_valid);

    /* Return worst error so caller can decide on retry. */
    if (r_isl != BMS_OK) { return r_isl; }
    return r_adc;
}

/* ── Pack measurement update (called from ISL/ADC drivers on success) ─────── */
void bms_measurements_update_pack(int32_t vbat_mv, int32_t vpack_mv,
                                   int32_t i_batt_ma,
                                   bool vbat_valid, bool vpack_valid,
                                   bool i_batt_valid) {
    s_pack.vbat_mv      = vbat_mv;
    s_pack.vpack_mv     = vpack_mv;
    s_pack.i_batt_ma    = i_batt_ma;
    s_pack.vbat_valid   = vbat_valid;
    s_pack.vpack_valid  = vpack_valid;
    s_pack.i_batt_valid = i_batt_valid;
    s_pack.timestamp_ms = board_clock_get_ms();
}

/* ── Accessors ────────────────────────────────────────────────────────────── */
const CellSnapshot    *bms_measurements_get_cells(void) { return &s_cells; }
const TempSnapshot    *bms_measurements_get_temps(void) { return &s_temps; }
const PackMeasurement *bms_measurements_get_pack(void)  { return &s_pack;  }
