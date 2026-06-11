/* test_bms_outputs.c — permission output gating unit tests. */
#include "unity.h"
#include "bms_outputs.h"
#include "board_outputs.h"

void setUp(void) {
    board_outputs_init_safe();
}

void tearDown(void) {}

static BmsPermissionRequest all_wanted(void) {
    return (BmsPermissionRequest){true, true, true, true};
}

void test_no_faults_all_permissions_granted(void) {
    BmsPermissionRequest req = all_wanted();
    bms_outputs_apply(&req, 0u, 0u);
    BmsOutputsBitmask s = bms_outputs_get_state();
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_MASTER_OK);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_DISCHARGE);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_CHARGE);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_CHARGER_SAFETY);
}

void test_fatal_fault_deasserts_all(void) {
    BmsPermissionRequest req = all_wanted();
    /* TEMP_CHAIN_BALANCE_ATTEMPT is FATAL — must trigger emergency deassert */
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT), 0u);
    TEST_ASSERT_EQUAL(0u, bms_outputs_get_state());
}

void test_watchdog_fault_blocks_all(void) {
    BmsPermissionRequest req = all_wanted();
    /* WATCHDOG is not in FAULT_FATAL_MASK (no boot-loop) but blocks all four
     * permissions via the blocking masks. */
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_WATCHDOG), 0u);
    TEST_ASSERT_EQUAL(0u, bms_outputs_get_state());
}

void test_latched_fault_blocks_even_when_not_active(void) {
    BmsPermissionRequest req = all_wanted();
    /* A latched CELL_UV (condition resolved, not yet cleared) must keep
     * discharge and master_ok blocked. */
    bms_outputs_apply(&req, 0u, FAULT_MASK(FAULT_BIT_CELL_UV));
    BmsOutputsBitmask s = bms_outputs_get_state();
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_DISCHARGE);
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_MASTER_OK);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_CHARGE);
}

void test_latched_fatal_fault_deasserts_all(void) {
    BmsPermissionRequest req = all_wanted();
    bms_outputs_apply(&req, 0u, FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT));
    TEST_ASSERT_EQUAL(0u, bms_outputs_get_state());
}

void test_all_blocking_fault_deasserts_all(void) {
    BmsPermissionRequest req = all_wanted();
    /* CONFIG_INVALID blocks all permissions (but is not FATAL — device still runs) */
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_CONFIG_INVALID), 0u);
    TEST_ASSERT_EQUAL(0u, bms_outputs_get_state());
}

void test_cell_uv_blocks_discharge_not_charge(void) {
    BmsPermissionRequest req = all_wanted();
    /* CELL_UV blocks discharge+master_ok but NOT charge (per fault_bits.yaml bit 1) */
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_CELL_UV), 0u);
    BmsOutputsBitmask s = bms_outputs_get_state();
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_DISCHARGE);
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_MASTER_OK);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_CHARGE);
}

void test_cell_ov_blocks_charge(void) {
    BmsPermissionRequest req = all_wanted();
    /* CELL_OV blocks all including charge */
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_CELL_OV), 0u);
    BmsOutputsBitmask s = bms_outputs_get_state();
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_CHARGE);
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_DISCHARGE);
}

void test_temp_over_charge_blocks_charge_not_discharge(void) {
    BmsPermissionRequest req = all_wanted();
    /* TEMP_OVER_CHARGE blocks charge+charger_safety but NOT discharge (bit 6) */
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_TEMP_OVER_CHARGE), 0u);
    BmsOutputsBitmask s = bms_outputs_get_state();
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_CHARGE);
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_CHARGER_SAFETY);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_DISCHARGE);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_MASTER_OK);
}

void test_temp_over_discharge_blocks_discharge_not_charge(void) {
    BmsPermissionRequest req = all_wanted();
    bms_outputs_apply(&req, FAULT_MASK(FAULT_BIT_TEMP_OVER_DISCHARGE), 0u);
    BmsOutputsBitmask s = bms_outputs_get_state();
    TEST_ASSERT_FALSE(s & OUTPUTS_BIT_DISCHARGE);
    TEST_ASSERT_TRUE(s & OUTPUTS_BIT_CHARGE);
}

void test_deassert_all_clears_all_permissions(void) {
    BmsPermissionRequest req = all_wanted();
    bms_outputs_apply(&req, 0u, 0u);
    TEST_ASSERT_NOT_EQUAL(0u, bms_outputs_get_state());
    bms_outputs_deassert_all();
    TEST_ASSERT_EQUAL(0u, bms_outputs_get_state());
}

void test_no_permission_wanted_no_output_set(void) {
    BmsPermissionRequest req = {false, false, false, false};
    bms_outputs_apply(&req, 0u, 0u);
    TEST_ASSERT_EQUAL(0u, bms_outputs_get_state());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_faults_all_permissions_granted);
    RUN_TEST(test_fatal_fault_deasserts_all);
    RUN_TEST(test_watchdog_fault_blocks_all);
    RUN_TEST(test_latched_fault_blocks_even_when_not_active);
    RUN_TEST(test_latched_fatal_fault_deasserts_all);
    RUN_TEST(test_all_blocking_fault_deasserts_all);
    RUN_TEST(test_cell_uv_blocks_discharge_not_charge);
    RUN_TEST(test_cell_ov_blocks_charge);
    RUN_TEST(test_temp_over_charge_blocks_charge_not_discharge);
    RUN_TEST(test_temp_over_discharge_blocks_discharge_not_charge);
    RUN_TEST(test_deassert_all_clears_all_permissions);
    RUN_TEST(test_no_permission_wanted_no_output_set);
    return UNITY_END();
}
