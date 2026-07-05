/* mock_meas_deps.c — controllable stubs for bms_measurements.c host tests.
 *
 * Tests set the s_* variables before calling bms_measurements_run_*_cycle(),
 * then inspect results via bms_measurements_get_*().
 */
#include "ltc6812.h"
#include "isl28022.h"
#include "board_adc.h"
#include "bms_config.h"
#include "bms_faults.h"
#include "bms_diagnostics.h"
#include "bms_types.h"
#include <string.h>
#include <stdbool.h>

/* ── Config stub ──────────────────────────────────────────────────────────── */
static BmsConfig s_cfg;

void bms_config_load_defaults(BmsConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cell_count          = TOTAL_CELL_COUNT;  /* full chain by default */
    cfg->temp_count          = TOTAL_TEMP_COUNT;
    cfg->temp_settle_time_ms = 5;
    cfg->vbat_gain_x1000     = 1000;
    cfg->vpack_gain_x1000    = 1000;
    cfg->current_gain_x1000  = 1000;
}

const BmsConfig *bms_config_get(void) { return &s_cfg; }

/* Mirror the real accessors (bms_config.c) so measurement/balance code under
 * test bounds its chain reads by the mock's configured count. */
uint8_t bms_config_active_cell_ics(void) {
    uint8_t ics = (uint8_t)(s_cfg.cell_count / CELLS_PER_IC);
    if (ics < MIN_CELL_IC_COUNT) { ics = MIN_CELL_IC_COUNT; }
    if (ics > CELL_IC_COUNT)     { ics = CELL_IC_COUNT; }
    return ics;
}

uint8_t bms_config_active_temp_ics(void) {
    uint8_t ics = (uint8_t)(s_cfg.temp_count / TEMPS_PER_IC);
    if (ics < MIN_CELL_IC_COUNT) { ics = MIN_CELL_IC_COUNT; }
    if (ics > TEMP_IC_COUNT)     { ics = TEMP_IC_COUNT; }
    return ics;
}

void mock_meas_config_init(void) { bms_config_load_defaults(&s_cfg); }

/* ── ltc6812 stubs (controllable) ────────────────────────────────────────── */
static BmsResult s_read_cells_result  = BMS_OK;
static uint16_t  s_read_cells_mv[CELL_IC_COUNT][CELLS_PER_IC];
static bool      s_read_cells_pec[CELL_IC_COUNT];
static BmsResult s_set_bias_result    = BMS_OK;
static BmsResult s_clear_s_result     = BMS_OK;

/* Call-count tracking (tests inspect these) */
int mock_set_bias_calls  = 0;
int mock_clear_s_calls   = 0;

BmsResult ltc6812_read_cells(BmsChain chain, uint8_t num_ics,
                              uint16_t mv_out[CELL_IC_COUNT][CELLS_PER_IC],
                              bool pec_ok[CELL_IC_COUNT]) {
    (void)chain;
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        pec_ok[ic] = s_read_cells_pec[ic];
        for (uint8_t c = 0; c < CELLS_PER_IC; c++) {
            mv_out[ic][c] = s_read_cells_mv[ic][c];
        }
    }
    return s_read_cells_result;
}

BmsResult ltc6812_temp_chain_set_sensor_bias(BmsChain chain, uint8_t num_ics,
                                              uint16_t s_mask) {
    (void)chain; (void)num_ics; (void)s_mask;
    mock_set_bias_calls++;
    return s_set_bias_result;
}

BmsResult ltc6812_temp_chain_clear_s_outputs(BmsChain chain, uint8_t num_ics) {
    (void)chain; (void)num_ics;
    mock_clear_s_calls++;
    return s_clear_s_result;
}

/* ── ISL28022 stub ────────────────────────────────────────────────────────── */
static BmsResult s_isl_result    = BMS_OK;
static int32_t   s_isl_vbus_mv   = 48000;
static int32_t   s_isl_vshunt_uv = 0;

BmsResult isl28022_read(int32_t *vbus_mv_out, int32_t *vshunt_uv_out) {
    *vbus_mv_out   = s_isl_vbus_mv;
    *vshunt_uv_out = s_isl_vshunt_uv;
    return s_isl_result;
}

/* ── ADC stub ─────────────────────────────────────────────────────────────── */
static BmsResult s_adc_result = BMS_OK;
static uint16_t  s_adc_raw    = 2048;

BmsResult board_adc_read_raw(uint16_t *raw_out) {
    *raw_out = s_adc_raw;
    return s_adc_result;
}

/* ── Fault stubs (observe calls) ──────────────────────────────────────────── */
bool mock_pec_error_reported[2];   /* [0]=cell [1]=temp */
bool mock_pec_counter_cleared[2];
bool mock_i2c_error_reported;
bool mock_i2c_error_cleared;

void bms_faults_report_pec_error(BmsChain chain) {
    mock_pec_error_reported[(int)chain] = true;
}
void bms_faults_clear_pec_counter(BmsChain chain) {
    mock_pec_counter_cleared[(int)chain] = true;
}
void bms_faults_report_i2c_error(void) { mock_i2c_error_reported = true; }
void bms_faults_clear_i2c_error(void)  { mock_i2c_error_cleared  = true; }

/* ── Diagnostics stub ─────────────────────────────────────────────────────── */
void bms_diagnostics_record_i2c_error(void) {}

/* ── Test-facing reset helpers ────────────────────────────────────────────── */
void mock_meas_reset(void) {
    mock_meas_config_init();
    s_read_cells_result = BMS_OK;
    s_set_bias_result   = BMS_OK;
    s_clear_s_result    = BMS_OK;
    s_isl_result        = BMS_OK;
    s_isl_vbus_mv       = 48000;
    s_isl_vshunt_uv     = 0;
    s_adc_result        = BMS_OK;
    s_adc_raw           = 2048;
    mock_set_bias_calls = 0;
    mock_clear_s_calls  = 0;
    mock_pec_error_reported[0]  = false;
    mock_pec_error_reported[1]  = false;
    mock_pec_counter_cleared[0] = false;
    mock_pec_counter_cleared[1] = false;
    mock_i2c_error_reported     = false;
    mock_i2c_error_cleared      = false;
    for (uint8_t ic = 0; ic < CELL_IC_COUNT; ic++) {
        s_read_cells_pec[ic] = true;
        for (uint8_t c = 0; c < CELLS_PER_IC; c++) {
            s_read_cells_mv[ic][c] = 3700;
        }
    }
}

/* Setters for tests */
void mock_set_read_cells_result(BmsResult r) { s_read_cells_result = r; }
void mock_set_read_cells_pec_all(bool ok) {
    for (uint8_t i = 0; i < CELL_IC_COUNT; i++) { s_read_cells_pec[i] = ok; }
}
void mock_set_read_cells_mv_all(uint16_t mv) {
    for (uint8_t ic = 0; ic < CELL_IC_COUNT; ic++) {
        for (uint8_t c = 0; c < CELLS_PER_IC; c++) {
            s_read_cells_mv[ic][c] = mv;
        }
    }
}
void mock_set_isl_result(BmsResult r) { s_isl_result = r; }
void mock_set_isl_vbus_mv(int32_t mv) { s_isl_vbus_mv = mv; }
void mock_set_adc_result(BmsResult r) { s_adc_result = r; }
void mock_set_adc_raw(uint16_t raw)   { s_adc_raw = raw; }
void mock_set_bias_result(BmsResult r)    { s_set_bias_result = r; }
void mock_set_clear_s_result(BmsResult r) { s_clear_s_result = r; }
