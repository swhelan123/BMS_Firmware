/* test_measurements_decode.c — bms_measurements unit tests.
 *
 * Tests cell/temp/pack measurement cycles using controllable ltc6812,
 * isl28022, and ADC stubs from mock_meas_deps.c.
 *
 * Safety invariant tested: TEMP chain S-outputs are ALWAYS cleared,
 * even when the SPI read fails.
 */
#include "unity.h"
#include "bms_measurements.h"
#include "bms_constants.h"
#include "bms_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Declarations of test-facing mock helpers ─────────────────────────────── */
extern int  mock_set_bias_calls;
extern int  mock_clear_s_calls;
extern bool mock_pec_error_reported[2];
extern bool mock_pec_counter_cleared[2];
extern bool mock_i2c_error_reported;
extern bool mock_i2c_error_cleared;

void mock_meas_reset(void);
void mock_set_read_cells_result(BmsResult r);
void mock_set_read_cells_pec_all(bool ok);
void mock_set_read_cells_mv_all(uint16_t mv);
void mock_set_isl_result(BmsResult r);
void mock_set_isl_vbus_mv(int32_t mv);
void mock_set_adc_result(BmsResult r);
void mock_set_adc_raw(uint16_t raw);
void mock_set_bias_result(BmsResult r);
void mock_set_clear_s_result(BmsResult r);

/* ── setUp / tearDown ─────────────────────────────────────────────────────── */

void setUp(void) {
    mock_meas_reset();
}

void tearDown(void) {}

/* ── Cell cycle tests ─────────────────────────────────────────────────────── */

void test_cell_cycle_ok_returns_ok(void) {
    BmsResult r = bms_measurements_run_cell_cycle();
    TEST_ASSERT_EQUAL(BMS_OK, r);
}

void test_cell_cycle_ok_overall_valid(void) {
    bms_measurements_run_cell_cycle();
    const CellSnapshot *c = bms_measurements_get_cells();
    TEST_ASSERT_EQUAL(MEAS_VALID, c->overall);
}

void test_cell_cycle_ok_all_cells_valid(void) {
    bms_measurements_run_cell_cycle();
    const CellSnapshot *c = bms_measurements_get_cells();
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_TRUE(c->valid[i]);
    }
}

void test_cell_cycle_stores_mv_values(void) {
    mock_set_read_cells_mv_all(3800);
    bms_measurements_run_cell_cycle();
    const CellSnapshot *c = bms_measurements_get_cells();
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT16(3800, c->mv[i]);
    }
}

void test_cell_cycle_pec_fail_overall_error(void) {
    mock_set_read_cells_result(BMS_ERR_PEC);
    mock_set_read_cells_pec_all(false);
    bms_measurements_run_cell_cycle();
    const CellSnapshot *c = bms_measurements_get_cells();
    TEST_ASSERT_EQUAL(MEAS_ERROR, c->overall);
}

void test_cell_cycle_pec_fail_all_invalid(void) {
    mock_set_read_cells_result(BMS_ERR_PEC);
    mock_set_read_cells_pec_all(false);
    bms_measurements_run_cell_cycle();
    const CellSnapshot *c = bms_measurements_get_cells();
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(c->valid[i]);
    }
}

void test_cell_cycle_pec_fail_reports_pec_error(void) {
    mock_set_read_cells_result(BMS_ERR_PEC);
    bms_measurements_run_cell_cycle();
    TEST_ASSERT_TRUE(mock_pec_error_reported[BMS_CHAIN_CELL]);
}

void test_cell_cycle_ok_clears_pec_counter(void) {
    bms_measurements_run_cell_cycle();
    TEST_ASSERT_TRUE(mock_pec_counter_cleared[BMS_CHAIN_CELL]);
}

/* ── Temperature cycle tests ──────────────────────────────────────────────── */

void test_temp_cycle_sets_bias_before_read(void) {
    bms_measurements_run_temp_cycle();
    /* Bias must be set exactly once */
    TEST_ASSERT_EQUAL_INT(1, mock_set_bias_calls);
}

void test_temp_cycle_clears_s_outputs_on_success(void) {
    bms_measurements_run_temp_cycle();
    TEST_ASSERT_EQUAL_INT(1, mock_clear_s_calls);
}

void test_temp_cycle_clears_s_outputs_on_bias_failure(void) {
    mock_set_bias_result(BMS_ERR_SPI);
    bms_measurements_run_temp_cycle();
    /* Must clear even when bias assertion fails */
    TEST_ASSERT_EQUAL_INT(1, mock_clear_s_calls);
}

void test_temp_cycle_clears_s_outputs_on_read_failure(void) {
    mock_set_read_cells_result(BMS_ERR_PEC);
    bms_measurements_run_temp_cycle();
    /* Must clear even when the ADCV read fails */
    TEST_ASSERT_EQUAL_INT(1, mock_clear_s_calls);
}

void test_temp_cycle_out_of_range_voltage_all_invalid(void) {
    /* Default mock voltage is 3700 mV, which exceeds table max (2440 mV).
     * All converted temperatures must be TEMP_INVALID_CX10. */
    bms_measurements_run_temp_cycle();
    const TempSnapshot *t = bms_measurements_get_temps();
    for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT16(TEMP_INVALID_CX10, t->cx10[i]);
    }
}

void test_temp_cycle_out_of_range_voltage_all_not_valid(void) {
    /* Default mock voltage 3700 mV is out of range → all valid[] must be false. */
    bms_measurements_run_temp_cycle();
    const TempSnapshot *t = bms_measurements_get_temps();
    for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
        TEST_ASSERT_FALSE(t->valid[i]);
    }
}

void test_temp_cycle_overall_valid_after_ok_read(void) {
    bms_measurements_run_temp_cycle();
    /* Even with ENEPAQ invalid, the read itself was OK → overall = MEAS_VALID */
    const TempSnapshot *t = bms_measurements_get_temps();
    TEST_ASSERT_EQUAL(MEAS_VALID, t->overall);
}

void test_temp_cycle_pec_fail_overall_error(void) {
    mock_set_read_cells_result(BMS_ERR_PEC);
    bms_measurements_run_temp_cycle();
    const TempSnapshot *t = bms_measurements_get_temps();
    TEST_ASSERT_EQUAL(MEAS_ERROR, t->overall);
}

void test_temp_cycle_pec_fail_reports_pec_error(void) {
    mock_set_read_cells_result(BMS_ERR_PEC);
    bms_measurements_run_temp_cycle();
    TEST_ASSERT_TRUE(mock_pec_error_reported[BMS_CHAIN_TEMP]);
}

void test_temp_cycle_ok_clears_pec_counter(void) {
    bms_measurements_run_temp_cycle();
    TEST_ASSERT_TRUE(mock_pec_counter_cleared[BMS_CHAIN_TEMP]);
}

/* ── Pack cycle tests ─────────────────────────────────────────────────────── */

void test_pack_cycle_ok_vbat_valid(void) {
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    TEST_ASSERT_TRUE(p->vbat_valid);
}

void test_pack_cycle_ok_vpack_valid(void) {
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    TEST_ASSERT_TRUE(p->vpack_valid);
}

void test_pack_cycle_ok_i_batt_valid(void) {
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    TEST_ASSERT_TRUE(p->i_batt_valid);
}

void test_pack_cycle_isl_fail_vbat_invalid(void) {
    mock_set_isl_result(BMS_ERR_I2C);
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    TEST_ASSERT_FALSE(p->vbat_valid);
    TEST_ASSERT_FALSE(p->i_batt_valid);
}

void test_pack_cycle_isl_fail_reports_i2c_error(void) {
    mock_set_isl_result(BMS_ERR_I2C);
    bms_measurements_run_pack_cycle();
    TEST_ASSERT_TRUE(mock_i2c_error_reported);
}

void test_pack_cycle_isl_ok_clears_i2c_error(void) {
    bms_measurements_run_pack_cycle();
    TEST_ASSERT_TRUE(mock_i2c_error_cleared);
}

void test_pack_cycle_adc_fail_vpack_invalid(void) {
    mock_set_adc_result(BMS_ERR_TIMEOUT);
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    TEST_ASSERT_FALSE(p->vpack_valid);
}

void test_pack_cycle_adc_ok_vpack_valid(void) {
    mock_set_adc_raw(2048);
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    TEST_ASSERT_TRUE(p->vpack_valid);
}

void test_pack_cycle_vbat_mv_stored(void) {
    mock_set_isl_vbus_mv(50000);
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    /* vbat_mv = raw_mv * gain/1000 + offset = 50000 * 1 + 0 = 50000 */
    TEST_ASSERT_EQUAL_INT32(50000, p->vbat_mv);
}

void test_pack_cycle_vpack_adc_nonzero(void) {
    mock_set_adc_raw(2048);
    bms_measurements_run_pack_cycle();
    const PackMeasurement *p = bms_measurements_get_pack();
    /* vpack_mv = (2048 * 3300 / 4096) * 1 = ~1650 mV */
    TEST_ASSERT_GREATER_THAN_INT32(0, p->vpack_mv);
}

/* ── Multiple cycles ──────────────────────────────────────────────────────── */

void test_second_cell_cycle_overwrites_first(void) {
    mock_set_read_cells_mv_all(3500);
    bms_measurements_run_cell_cycle();
    mock_set_read_cells_mv_all(4000);
    bms_measurements_run_cell_cycle();
    const CellSnapshot *c = bms_measurements_get_cells();
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT16(4000, c->mv[i]);
    }
}

void test_temp_cycle_s_outputs_cleared_each_call(void) {
    bms_measurements_run_temp_cycle();
    bms_measurements_run_temp_cycle();
    TEST_ASSERT_EQUAL_INT(2, mock_clear_s_calls);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Cell cycle */
    RUN_TEST(test_cell_cycle_ok_returns_ok);
    RUN_TEST(test_cell_cycle_ok_overall_valid);
    RUN_TEST(test_cell_cycle_ok_all_cells_valid);
    RUN_TEST(test_cell_cycle_stores_mv_values);
    RUN_TEST(test_cell_cycle_pec_fail_overall_error);
    RUN_TEST(test_cell_cycle_pec_fail_all_invalid);
    RUN_TEST(test_cell_cycle_pec_fail_reports_pec_error);
    RUN_TEST(test_cell_cycle_ok_clears_pec_counter);

    /* Temperature cycle */
    RUN_TEST(test_temp_cycle_sets_bias_before_read);
    RUN_TEST(test_temp_cycle_clears_s_outputs_on_success);
    RUN_TEST(test_temp_cycle_clears_s_outputs_on_bias_failure);
    RUN_TEST(test_temp_cycle_clears_s_outputs_on_read_failure);
    RUN_TEST(test_temp_cycle_out_of_range_voltage_all_invalid);
    RUN_TEST(test_temp_cycle_out_of_range_voltage_all_not_valid);
    RUN_TEST(test_temp_cycle_overall_valid_after_ok_read);
    RUN_TEST(test_temp_cycle_pec_fail_overall_error);
    RUN_TEST(test_temp_cycle_pec_fail_reports_pec_error);
    RUN_TEST(test_temp_cycle_ok_clears_pec_counter);

    /* Pack cycle */
    RUN_TEST(test_pack_cycle_ok_vbat_valid);
    RUN_TEST(test_pack_cycle_ok_vpack_valid);
    RUN_TEST(test_pack_cycle_ok_i_batt_valid);
    RUN_TEST(test_pack_cycle_isl_fail_vbat_invalid);
    RUN_TEST(test_pack_cycle_isl_fail_reports_i2c_error);
    RUN_TEST(test_pack_cycle_isl_ok_clears_i2c_error);
    RUN_TEST(test_pack_cycle_adc_fail_vpack_invalid);
    RUN_TEST(test_pack_cycle_adc_ok_vpack_valid);
    RUN_TEST(test_pack_cycle_vbat_mv_stored);
    RUN_TEST(test_pack_cycle_vpack_adc_nonzero);

    /* Multiple cycles */
    RUN_TEST(test_second_cell_cycle_overwrites_first);
    RUN_TEST(test_temp_cycle_s_outputs_cleared_each_call);

    return UNITY_END();
}
