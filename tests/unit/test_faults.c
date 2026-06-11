/* test_faults.c — bms_faults_evaluate unit tests.
 * Tests fault bit evaluation from measurement data and config thresholds.
 */
#include "unity.h"
#include "bms_faults.h"
#include "bms_config.h"
#include "bms_constants.h"
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static BmsConfig s_cfg;
static CellSnapshot    s_cells;
static TempSnapshot    s_temps;
static PackMeasurement s_pack;

static void make_valid_cells(uint16_t mv_each) {
    s_cells.timestamp_ms = 0;
    s_cells.overall      = MEAS_VALID;
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        s_cells.mv[i]    = mv_each;
        s_cells.valid[i] = true;
    }
}

static void make_valid_temps(int16_t cx10_each) {
    s_temps.timestamp_ms = 0;
    s_temps.overall      = MEAS_VALID;
    for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
        s_temps.cx10[i]  = cx10_each;
        s_temps.valid[i] = true;
    }
}

static void make_valid_pack(void) {
    s_pack.timestamp_ms = 0;
    s_pack.vbat_mv      = 48000;
    s_pack.vpack_mv     = 48000;
    s_pack.i_batt_ma    = 0;
    s_pack.vbat_valid   = true;
    s_pack.vpack_valid  = true;
    s_pack.i_batt_valid = true;
}

static void defaults_config(void) {
    bms_config_load_defaults(&s_cfg);
    /* Enable all cells and temps in required masks */
    for (uint8_t i = 0; i < CONFIG_MASK_BYTES; i++) {
        s_cfg.required_cell_mask[i] = 0xFFu;
        s_cfg.required_temp_mask[i] = 0xFFu;
    }
    /* Top byte: only low 3 bits valid (cells 72-74) */
    s_cfg.required_cell_mask[9] = 0x07u;
    s_cfg.required_temp_mask[9] = 0x07u;
}

void setUp(void) {
    defaults_config();
    make_valid_cells(3700);
    make_valid_temps(250);
    make_valid_pack();
    /* Reset fault state */
    bms_faults_clear_pec_counter(BMS_CHAIN_CELL);
    bms_faults_clear_pec_counter(BMS_CHAIN_TEMP);
    bms_faults_clear_i2c_error();
    /* Evaluate once with good data to clear any latched faults */
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
}

void tearDown(void) {}

/* ── Cell voltage fault tests ─────────────────────────────────────────────── */

void test_no_faults_nominal_cells(void) {
    make_valid_cells(3700);
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    uint64_t af = bms_faults_get_active();
    TEST_ASSERT_FALSE(af & FAULT_MASK(FAULT_BIT_CELL_UV));
    TEST_ASSERT_FALSE(af & FAULT_MASK(FAULT_BIT_CELL_OV));
    TEST_ASSERT_FALSE(af & FAULT_MASK(FAULT_BIT_CELL_UV_SOFT));
    TEST_ASSERT_FALSE(af & FAULT_MASK(FAULT_BIT_CELL_OV_SOFT));
}

void test_cell_uv_hard_detected(void) {
    make_valid_cells(3700);
    s_cells.mv[0] = s_cfg.cell_uv_hard_mv - 1u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_UV));
}

void test_cell_ov_hard_detected(void) {
    make_valid_cells(3700);
    s_cells.mv[0] = s_cfg.cell_ov_hard_mv + 1u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_OV));
}

void test_cell_uv_soft_detected(void) {
    make_valid_cells(3700);
    s_cells.mv[0] = (uint16_t)(s_cfg.cell_uv_soft_mv - 1u);
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_UV_SOFT));
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_UV));
}

void test_cell_not_in_required_mask_ignored(void) {
    /* Clear all required mask bits */
    memset(s_cfg.required_cell_mask, 0, CONFIG_MASK_BYTES);
    make_valid_cells(1000); /* would be severe UV if checked */
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    /* No UV fault because no cells are in the required mask */
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_UV));
}

void test_invalid_cell_measurement_ignored(void) {
    make_valid_cells(3700);
    s_cells.mv[0]    = 1000; /* would be UV */
    s_cells.valid[0] = false;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_UV));
}

/* ── Stale data fault tests ───────────────────────────────────────────────── */

void test_stale_cell_data_sets_read_invalid(void) {
    make_valid_cells(3700);
    /* Make data stale by setting timestamp far in the past.
     * mock_clock returns 0; stale_data_timeout_ms = default.
     * To make it stale: set timestamp such that now - ts > timeout.
     * Since mock clock returns 0, set timestamp = UINT32_MAX - 1 to wrap around. */
    s_cells.timestamp_ms = 0; /* now=0, ts=0 → elapsed=0, not stale */
    /* Actually set timestamp before now by a large amount */
    s_cells.timestamp_ms = (uint32_t)(-(int32_t)(s_cfg.stale_data_timeout_ms + 1u));
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_READ_INVALID));
}

/* ── Temperature fault tests ─────────────────────────────────────────────── */

void test_temp_over_charge_detected(void) {
    make_valid_temps((int16_t)(s_cfg.temp_charge_hard_cx10 + 5));
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_TEMP_OVER_CHARGE));
}

void test_temp_over_discharge_detected(void) {
    make_valid_temps((int16_t)(s_cfg.temp_discharge_hard_cx10 + 5));
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_TEMP_OVER_DISCHARGE));
}

void test_temp_nominal_no_faults(void) {
    make_valid_temps(250); /* 25.0°C — well within limits */
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_TEMP_OVER_CHARGE));
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_TEMP_OVER_DISCHARGE));
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_TEMP_OVER_ABS));
}

/* ── PEC / isoSPI escalation tests ───────────────────────────────────────── */

void test_pec_escalation_to_isospi_fault(void) {
    /* Report LTC6812_MAX_RETRIES consecutive PEC errors → ISOSPI fault */
    for (uint8_t i = 0; i < LTC6812_MAX_RETRIES; i++) {
        bms_faults_report_pec_error(BMS_CHAIN_CELL);
    }
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_ISOSPI_CELL));
}

void test_pec_clear_removes_isospi_fault(void) {
    for (uint8_t i = 0; i < LTC6812_MAX_RETRIES; i++) {
        bms_faults_report_pec_error(BMS_CHAIN_CELL);
    }
    TEST_ASSERT_TRUE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_ISOSPI_CELL));
    bms_faults_clear_pec_counter(BMS_CHAIN_CELL);
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_ISOSPI_CELL));
}

void test_pec_below_threshold_no_isospi_fault(void) {
    /* One less than threshold: no escalation yet */
    for (uint8_t i = 0; i < LTC6812_MAX_RETRIES - 1u; i++) {
        bms_faults_report_pec_error(BMS_CHAIN_CELL);
    }
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_ISOSPI_CELL));
}

void test_pec_error_only_affects_matching_chain(void) {
    for (uint8_t i = 0; i < LTC6812_MAX_RETRIES; i++) {
        bms_faults_report_pec_error(BMS_CHAIN_TEMP);
    }
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_ISOSPI_CELL));
    TEST_ASSERT_TRUE(bms_faults_get_active()  & FAULT_MASK(FAULT_BIT_ISOSPI_TEMP));
}

/* ── Latching tests ──────────────────────────────────────────────────────── */

void test_latched_fault_persists_after_condition_clears(void) {
    make_valid_cells(3700);
    s_cells.mv[0] = s_cfg.cell_uv_hard_mv - 1u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_CELL_UV));

    /* Restore normal voltage — active fault clears, latched persists */
    s_cells.mv[0] = 3700u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_UV));
    TEST_ASSERT_TRUE(bms_faults_get_latched()  & FAULT_MASK(FAULT_BIT_CELL_UV));
}

void test_clear_latched_only_when_active_resolved(void) {
    s_cells.mv[0] = s_cfg.cell_uv_hard_mv - 1u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    /* Active still set — clear should be rejected */
    uint64_t cleared = bms_faults_clear_latched(FAULT_MASK(FAULT_BIT_CELL_UV));
    TEST_ASSERT_EQUAL_UINT64(0u, cleared);
    TEST_ASSERT_TRUE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_CELL_UV));
}

void test_clear_latched_succeeds_after_active_resolves(void) {
    s_cells.mv[0] = s_cfg.cell_uv_hard_mv - 1u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    s_cells.mv[0] = 3700u;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    uint64_t cleared = bms_faults_clear_latched(FAULT_MASK(FAULT_BIT_CELL_UV));
    TEST_ASSERT_NOT_EQUAL(0u, cleared & FAULT_MASK(FAULT_BIT_CELL_UV));
    TEST_ASSERT_FALSE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_CELL_UV));
}

void test_nonlatching_fault_does_not_latch(void) {
    /* VBAT_INVALID is latching:false — must track active condition only */
    s_pack.vbat_valid = false;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active()  & FAULT_MASK(FAULT_BIT_VBAT_INVALID));
    TEST_ASSERT_FALSE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_VBAT_INVALID));

    s_pack.vbat_valid = true;
    bms_faults_evaluate(&s_cells, &s_temps, &s_pack, &s_cfg);
    TEST_ASSERT_FALSE(bms_faults_get_active()  & FAULT_MASK(FAULT_BIT_VBAT_INVALID));
    TEST_ASSERT_FALSE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_VBAT_INVALID));
}

void test_set_latched_is_latched_not_active(void) {
    bms_faults_set_latched(FAULT_BIT_WATCHDOG);
    TEST_ASSERT_FALSE(bms_faults_get_active()  & FAULT_MASK(FAULT_BIT_WATCHDOG));
    TEST_ASSERT_TRUE(bms_faults_get_latched()  & FAULT_MASK(FAULT_BIT_WATCHDOG));
    /* Clearable because the active condition is not present */
    uint64_t cleared = bms_faults_clear_latched(FAULT_MASK(FAULT_BIT_WATCHDOG));
    TEST_ASSERT_NOT_EQUAL(0u, cleared & FAULT_MASK(FAULT_BIT_WATCHDOG));
    TEST_ASSERT_FALSE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_WATCHDOG));
}

void test_openwire_on_required_cell_latches(void) {
    bool detected[TOTAL_CELL_COUNT];
    memset(detected, 0, sizeof(detected));
    detected[3] = true;  /* cell 3 is in the required mask (setUp enables all) */
    bms_faults_apply_openwire(detected, &s_cfg);
    TEST_ASSERT_TRUE(bms_faults_get_active()  & FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));
    TEST_ASSERT_TRUE(bms_faults_get_latched() & FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));

    /* Clean rescan clears active; latched persists until explicit clear */
    memset(detected, 0, sizeof(detected));
    bms_faults_apply_openwire(detected, &s_cfg);
    TEST_ASSERT_FALSE(bms_faults_get_active()  & FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));
    TEST_ASSERT_TRUE(bms_faults_get_latched()  & FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));
    uint64_t cleared = bms_faults_clear_latched(FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));
    TEST_ASSERT_NOT_EQUAL(0u, cleared & FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));
}

void test_openwire_on_unrequired_cell_ignored(void) {
    bool detected[TOTAL_CELL_COUNT];
    memset(detected, 0, sizeof(detected));
    memset(s_cfg.required_cell_mask, 0, CONFIG_MASK_BYTES);
    detected[3] = true;  /* not in required mask → no fault */
    bms_faults_apply_openwire(detected, &s_cfg);
    TEST_ASSERT_FALSE(bms_faults_get_active() & FAULT_MASK(FAULT_BIT_CELL_OPENWIRE));
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_faults_nominal_cells);
    RUN_TEST(test_cell_uv_hard_detected);
    RUN_TEST(test_cell_ov_hard_detected);
    RUN_TEST(test_cell_uv_soft_detected);
    RUN_TEST(test_cell_not_in_required_mask_ignored);
    RUN_TEST(test_invalid_cell_measurement_ignored);
    RUN_TEST(test_stale_cell_data_sets_read_invalid);
    RUN_TEST(test_temp_over_charge_detected);
    RUN_TEST(test_temp_over_discharge_detected);
    RUN_TEST(test_temp_nominal_no_faults);
    RUN_TEST(test_pec_escalation_to_isospi_fault);
    RUN_TEST(test_pec_clear_removes_isospi_fault);
    RUN_TEST(test_pec_below_threshold_no_isospi_fault);
    RUN_TEST(test_pec_error_only_affects_matching_chain);
    RUN_TEST(test_latched_fault_persists_after_condition_clears);
    RUN_TEST(test_clear_latched_only_when_active_resolved);
    RUN_TEST(test_clear_latched_succeeds_after_active_resolves);
    RUN_TEST(test_nonlatching_fault_does_not_latch);
    RUN_TEST(test_set_latched_is_latched_not_active);
    RUN_TEST(test_openwire_on_required_cell_latches);
    RUN_TEST(test_openwire_on_unrequired_cell_ignored);
    return UNITY_END();
}
