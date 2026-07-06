/* test_bms_charger.c — Elcon/TC Charger CAN control unit tests (BMS_HOST_BUILD).
 *
 * Provides its own minimal bms_measurements_get_cells() stub (a plain static
 * CellSnapshot the tests populate directly) rather than linking the full
 * measurements+ltc6812 mock stack — bms_charger.c only reads cell mv/valid,
 * nothing else from that module.
 */
#include "unity.h"
#include "bms_charger.h"
#include "mock_board_can.h"
#include <string.h>

/* ── Local cell-snapshot stub ─────────────────────────────────────────────── */
static CellSnapshot s_cells;

const CellSnapshot *bms_measurements_get_cells(void) { return &s_cells; }

static void set_cell(uint8_t idx, uint16_t mv, bool valid) {
    s_cells.mv[idx] = mv;
    s_cells.valid[idx] = valid;
}

static void reset_cells(void) {
    memset(&s_cells, 0, sizeof(s_cells));
}

/* Clock control (mock_board_clock.c) */
extern void mock_clock_advance_ms(uint32_t ms);
extern void board_clock_init(void); /* resets the mock clock to 0 */

/* ── Test config ──────────────────────────────────────────────────────────── */
static BmsConfig make_test_config(void) {
    BmsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cell_count = 75u;
    for (int i = 0; i < 10; i++) { cfg.required_cell_mask[i] = 0xFFu; }
    cfg.required_cell_mask[9] = 0x07u; /* bits 75-79 clear */
    cfg.cell_ov_soft_mv = 4150u;
    cfg.overcurrent_hard_ma = 210000u;
    cfg.charge_voltage_setpoint_dv = 3110u;
    cfg.charge_voltage_max_dv      = 3150u;
    cfg.charge_current_setpoint_da = 100u;
    cfg.charge_taper_current_da    = 10u;
    cfg.charge_taper_hold_ms       = 5000u;
    return cfg;
}

void setUp(void) {
    mock_can_reset();
    reset_cells();
    board_clock_init(); /* deterministic now=0 per test */
}
void tearDown(void) {}

/* ── Pure encode/decode ───────────────────────────────────────────────────── */

void test_build_command_matches_worked_example(void) {
    /* User-supplied reference: 311.0V / 10.0A / charge → 0C 26 00 64 00 00 00 00 */
    uint8_t frame[8];
    bms_charger_build_command(frame, 3110u, 100u, CHARGER_CTRL_CHARGE);
    uint8_t expected[8] = {0x0C, 0x26, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_MEMORY(expected, frame, 8);
}

void test_build_command_stop_control_byte(void) {
    uint8_t frame[8];
    bms_charger_build_command(frame, 0u, 0u, CHARGER_CTRL_STOP);
    TEST_ASSERT_EQUAL_UINT8(0x01u, frame[4]);
}

void test_parse_status_decodes_fields(void) {
    /* 311.0V, 5.5A, hardware-failure + over-temp flags */
    uint8_t frame[8] = {0x0C, 0x26, 0x00, 0x37, 0x03, 0x00, 0x00, 0x00};
    ChargerStatus st;
    TEST_ASSERT_TRUE(bms_charger_parse_status(frame, 8u, &st));
    TEST_ASSERT_EQUAL_UINT16(3110u, st.output_voltage_dv);
    TEST_ASSERT_EQUAL_UINT16(55u,   st.output_current_da);
    TEST_ASSERT_EQUAL_UINT8(0x03u,  st.status_flags);
    TEST_ASSERT_TRUE(st.status_valid);
}

void test_parse_status_rejects_short_frame(void) {
    uint8_t frame[4] = {0, 0, 0, 0};
    ChargerStatus st;
    TEST_ASSERT_FALSE(bms_charger_parse_status(frame, 4u, &st));
}

void test_taper_complete_requires_both_low_current_and_hold_time(void) {
    BmsConfig cfg = make_test_config();
    TEST_ASSERT_FALSE(bms_charger_taper_complete(&cfg, 5u, 4999u));  /* held not long enough */
    TEST_ASSERT_TRUE(bms_charger_taper_complete(&cfg, 5u, 5000u));   /* held exactly long enough */
    TEST_ASSERT_FALSE(bms_charger_taper_complete(&cfg, 10u, 10000u)); /* current at threshold, not below */
    TEST_ASSERT_FALSE(bms_charger_taper_complete(&cfg, 50u, 10000u)); /* current well above threshold */
}

/* ── Stateful control ─────────────────────────────────────────────────────── */

void test_start_switches_can_to_charge_mode(void) {
    bms_charger_start();
    TEST_ASSERT_TRUE(mock_can_get_charge_mode());
}

void test_stop_sends_stop_command_and_returns_to_drive_mode(void) {
    bms_charger_start();
    bms_charger_stop();
    TEST_ASSERT_FALSE(mock_can_get_charge_mode());

    uint32_t id; bool ext; uint8_t data[8]; uint8_t len;
    TEST_ASSERT_TRUE(mock_can_get_last_tx(&id, &ext, data, &len));
    TEST_ASSERT_EQUAL_UINT32(CHARGER_CAN_CMD_ID, id);
    TEST_ASSERT_TRUE(ext);
    TEST_ASSERT_EQUAL_UINT8(CHARGER_CTRL_STOP, data[4]);
}

void test_tick_sends_command_clamped_to_voltage_max(void) {
    BmsConfig cfg = make_test_config();
    cfg.charge_voltage_setpoint_dv = 9999u; /* deliberately above max */
    cfg.charge_voltage_max_dv      = 3150u;

    bms_charger_start();
    bms_charger_tick(&cfg); /* start() backdates the heartbeat so this sends immediately */

    uint32_t id; bool ext; uint8_t data[8]; uint8_t len;
    TEST_ASSERT_TRUE(mock_can_get_last_tx(&id, &ext, data, &len));
    TEST_ASSERT_EQUAL_UINT32(CHARGER_CAN_CMD_ID, id);
    uint16_t voltage = ((uint16_t)data[0] << 8) | data[1];
    TEST_ASSERT_EQUAL_UINT16(3150u, voltage); /* clamped, not the 9999 setpoint */
    TEST_ASSERT_EQUAL_UINT8(CHARGER_CTRL_CHARGE, data[4]);
}

void test_tick_does_not_resend_before_period_elapses(void) {
    BmsConfig cfg = make_test_config();
    bms_charger_start();
    bms_charger_tick(&cfg);
    uint32_t count_after_first = mock_can_get_tx_count();

    mock_clock_advance_ms(500u); /* half the 1000ms period */
    bms_charger_tick(&cfg);
    TEST_ASSERT_EQUAL_UINT32(count_after_first, mock_can_get_tx_count());

    mock_clock_advance_ms(600u); /* now past the period */
    bms_charger_tick(&cfg);
    TEST_ASSERT_EQUAL_UINT32(count_after_first + 1u, mock_can_get_tx_count());
}

void test_termination_on_required_cell_soft_ov(void) {
    BmsConfig cfg = make_test_config();
    bms_charger_start();
    TEST_ASSERT_FALSE(bms_charger_termination_requested());

    set_cell(0, cfg.cell_ov_soft_mv, true); /* required cell at soft-OV */
    bms_charger_tick(&cfg);

    TEST_ASSERT_TRUE(bms_charger_termination_requested());
}

void test_no_termination_from_unrequired_or_invalid_cell_at_ov(void) {
    BmsConfig cfg = make_test_config();
    cfg.required_cell_mask[0] &= (uint8_t)~0x01u; /* cell 0 no longer required */
    bms_charger_start();

    set_cell(0, cfg.cell_ov_soft_mv, true);   /* at OV, but not required */
    bms_charger_tick(&cfg);
    TEST_ASSERT_FALSE(bms_charger_termination_requested());

    set_cell(1, cfg.cell_ov_soft_mv, false);  /* required, but invalid reading */
    bms_charger_tick(&cfg);
    TEST_ASSERT_FALSE(bms_charger_termination_requested());
}

void test_termination_on_taper_held_long_enough(void) {
    BmsConfig cfg = make_test_config();
    bms_charger_start();

    uint8_t status[8];
    bms_charger_build_command(status, 3110u, 5u, 0u); /* reuse encoder: 0.5A output */
    mock_can_inject_rx(CHARGER_CAN_STATUS_ID, true, status, 8u);
    bms_charger_tick(&cfg); /* consumes the status frame, starts the taper timer */
    TEST_ASSERT_FALSE(bms_charger_termination_requested());

    mock_clock_advance_ms(4999u);
    bms_charger_tick(&cfg);
    TEST_ASSERT_FALSE(bms_charger_termination_requested()); /* not held long enough yet */

    mock_clock_advance_ms(2u);
    bms_charger_tick(&cfg);
    TEST_ASSERT_TRUE(bms_charger_termination_requested());
}

void test_taper_resets_if_current_rises_above_threshold(void) {
    BmsConfig cfg = make_test_config();
    bms_charger_start();

    uint8_t low[8];
    bms_charger_build_command(low, 3110u, 5u, 0u); /* 0.5A — below 1.0A taper threshold */
    mock_can_inject_rx(CHARGER_CAN_STATUS_ID, true, low, 8u);
    bms_charger_tick(&cfg);

    mock_clock_advance_ms(4000u); /* into the taper hold, but not complete */

    uint8_t high[8];
    bms_charger_build_command(high, 3110u, 50u, 0u); /* 5.0A — back above threshold */
    mock_can_inject_rx(CHARGER_CAN_STATUS_ID, true, high, 8u);
    bms_charger_tick(&cfg);

    mock_clock_advance_ms(1500u); /* would have completed the ORIGINAL hold window */
    bms_charger_tick(&cfg);
    TEST_ASSERT_FALSE(bms_charger_termination_requested()); /* timer was reset */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_build_command_matches_worked_example);
    RUN_TEST(test_build_command_stop_control_byte);
    RUN_TEST(test_parse_status_decodes_fields);
    RUN_TEST(test_parse_status_rejects_short_frame);
    RUN_TEST(test_taper_complete_requires_both_low_current_and_hold_time);
    RUN_TEST(test_start_switches_can_to_charge_mode);
    RUN_TEST(test_stop_sends_stop_command_and_returns_to_drive_mode);
    RUN_TEST(test_tick_sends_command_clamped_to_voltage_max);
    RUN_TEST(test_tick_does_not_resend_before_period_elapses);
    RUN_TEST(test_termination_on_required_cell_soft_ov);
    RUN_TEST(test_no_termination_from_unrequired_or_invalid_cell_at_ov);
    RUN_TEST(test_termination_on_taper_held_long_enough);
    RUN_TEST(test_taper_resets_if_current_rises_above_threshold);
    return UNITY_END();
}
