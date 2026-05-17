/* test_enepaq_table.c — Enepaq V-T table conversion tests.
 *
 * enepaq_voltage_to_cx10() is a static function inside bms_measurements.c;
 * it is exercised indirectly by injecting specific voltages into the mock
 * LTC6812 read path and inspecting the resulting TempSnapshot.
 *
 * Table source: Sony-Murata NTC Table 5, 33 breakpoints, -40°C to +120°C.
 * All 33 breakpoints produce exact (zero-interpolation-error) results.
 */
#include "unity.h"
#include "bms_measurements.h"
#include "bms_constants.h"
#include "bms_types.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Declarations of mock helpers (from mock_meas_deps.c) ─────────────────── */
void mock_meas_reset(void);
void mock_set_read_cells_result(BmsResult r);
void mock_set_read_cells_pec_all(bool ok);
void mock_set_read_cells_mv_all(uint16_t mv);

/* ── setUp / tearDown ─────────────────────────────────────────────────────── */

void setUp(void)    { mock_meas_reset(); }
void tearDown(void) {}

/* ── Helper ───────────────────────────────────────────────────────────────── */

/* Run a temp cycle with all sensors at voltage mv; return cx10 of sensor 0. */
static int16_t convert(uint16_t mv) {
    mock_set_read_cells_mv_all(mv);
    bms_measurements_run_temp_cycle();
    return bms_measurements_get_temps()->cx10[0];
}

/* ── Table endpoints ─────────────────────────────────────────────────────── */

void test_endpoint_low_temp_minus40(void) {
    /* 2440 mV → −40°C = −400 cx10 (first table entry, exact) */
    TEST_ASSERT_EQUAL_INT16(-400, convert(2440));
}

void test_endpoint_high_temp_plus120(void) {
    /* 1300 mV → +120°C = +1200 cx10 (last table entry, exact) */
    TEST_ASSERT_EQUAL_INT16(1200, convert(1300));
}

/* ── Known table breakpoints ─────────────────────────────────────────────── */

void test_known_point_0degC(void) {
    /* 2170 mV → 0°C = 0 cx10 */
    TEST_ASSERT_EQUAL_INT16(0, convert(2170));
}

void test_known_point_25degC(void) {
    /* 1860 mV → +25°C = +250 cx10 */
    TEST_ASSERT_EQUAL_INT16(250, convert(1860));
}

void test_known_point_60degC(void) {
    /* 1510 mV → +60°C = +600 cx10 */
    TEST_ASSERT_EQUAL_INT16(600, convert(1510));
}

void test_known_point_minus10degC(void) {
    /* 2270 mV → −10°C = −100 cx10 */
    TEST_ASSERT_EQUAL_INT16(-100, convert(2270));
}

/* ── Piecewise-linear interpolation ────────────────────────────────────────── */

void test_interpolation_between_minus40_and_minus35(void) {
    /* 2430 mV is midway (by absolute offset 10/20) between:
     *   2440 mV (−400 cx10) and 2420 mV (−350 cx10).
     * Expected: −400 + (10 × 50) / 20 = −375 cx10 */
    TEST_ASSERT_EQUAL_INT16(-375, convert(2430));
}

void test_interpolation_between_25_and_30degC(void) {
    /* 1830 mV sits between 1860 mV (+250 cx10) and 1800 mV (+300 cx10).
     * Expected: 250 + (30 × 50) / 60 = +275 cx10 */
    TEST_ASSERT_EQUAL_INT16(275, convert(1830));
}

/* ── Out-of-range inputs → TEMP_INVALID_CX10 ────────────────────────────── */

void test_voltage_above_max_is_invalid(void) {
    /* 2441 mV > 2440 mV (table max) */
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID_CX10, convert(2441));
}

void test_voltage_well_above_max_is_invalid(void) {
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID_CX10, convert(3700));
}

void test_voltage_below_min_is_invalid(void) {
    /* 1299 mV < 1300 mV (table min) */
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID_CX10, convert(1299));
}

void test_voltage_zero_is_invalid(void) {
    TEST_ASSERT_EQUAL_INT16(TEMP_INVALID_CX10, convert(0));
}

/* ── valid[] flag matches conversion result ───────────────────────────────── */

void test_in_range_voltage_marks_sensor_valid(void) {
    mock_set_read_cells_mv_all(1860);   /* +25°C, in range */
    bms_measurements_run_temp_cycle();
    TEST_ASSERT_TRUE(bms_measurements_get_temps()->valid[0]);
}

void test_out_of_range_voltage_marks_sensor_invalid(void) {
    mock_set_read_cells_mv_all(3700);   /* > 2440 mV, out of range */
    bms_measurements_run_temp_cycle();
    TEST_ASSERT_FALSE(bms_measurements_get_temps()->valid[0]);
}

/* ── All sensors converted uniformly ────────────────────────────────────────── */

void test_all_sensors_converted_at_25degC(void) {
    mock_set_read_cells_mv_all(1860);
    bms_measurements_run_temp_cycle();
    const TempSnapshot *t = bms_measurements_get_temps();
    for (uint8_t i = 0u; i < TOTAL_TEMP_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT16(250, t->cx10[i]);
    }
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_endpoint_low_temp_minus40);
    RUN_TEST(test_endpoint_high_temp_plus120);
    RUN_TEST(test_known_point_0degC);
    RUN_TEST(test_known_point_25degC);
    RUN_TEST(test_known_point_60degC);
    RUN_TEST(test_known_point_minus10degC);
    RUN_TEST(test_interpolation_between_minus40_and_minus35);
    RUN_TEST(test_interpolation_between_25_and_30degC);
    RUN_TEST(test_voltage_above_max_is_invalid);
    RUN_TEST(test_voltage_well_above_max_is_invalid);
    RUN_TEST(test_voltage_below_min_is_invalid);
    RUN_TEST(test_voltage_zero_is_invalid);
    RUN_TEST(test_in_range_voltage_marks_sensor_valid);
    RUN_TEST(test_out_of_range_voltage_marks_sensor_invalid);
    RUN_TEST(test_all_sensors_converted_at_25degC);

    return UNITY_END();
}
