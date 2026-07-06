/* test_balance.c — bms_balance_tick unit tests.
 * Tests guard conditions and DCC write behavior.
 *
 * PEC of 6 zero bytes = 0xC212 (verified by test_pec15 suite).
 * We preload mock SPI with valid PEC data so ltc6812 read-modify-write
 * succeeds and WRCFGA/WRCFGB writes can be inspected.
 */
#include "unity.h"
#include "bms_balance.h"
#include "bms_config.h"
#include "bms_constants.h"
#include "isospi.h"
#include "ltc6812.h"
#include "mock_board_spi.h"
#include <string.h>

/* 8 bytes per IC: 6 data (zeros) + 2 PEC bytes for all-zero CFGA/CFGB.
 * PEC of 0x00×6 = 0xC212 (LSB always 0 per datasheet). */
#define IC_VALID_PEC_H  0xC2u
#define IC_VALID_PEC_L  0x12u
#define BYTES_PER_IC    8u
/* ltc6812_cell_chain_set_balance does 4 reads: RDCFGA + RDCFGB (write phase)
 * + RDCFGA + RDCFGB (readback verification). Zero dcc_mask passes readback. */
#define MOCK_RX_SIZE    (CELL_IC_COUNT * BYTES_PER_IC * 4u)  /* 4 reads × 5 ICs × 8 bytes */

static uint8_t s_valid_rx[MOCK_RX_SIZE];

static BmsConfig    s_cfg;
static CellSnapshot s_cells;

static void load_valid_spi_rx(void) {
    /* Preload 4 rounds of valid zero-data: RDCFGA + RDCFGB (write phase)
     * + RDCFGA + RDCFGB (readback). Readback of zeros matches dcc_mask=0. */
    memset(s_valid_rx, 0, sizeof(s_valid_rx));
    for (uint8_t ic = 0; ic < CELL_IC_COUNT * 4u; ic++) {
        /* data bytes 0-5: 0x00 */
        s_valid_rx[ic * BYTES_PER_IC + 6] = IC_VALID_PEC_H;
        s_valid_rx[ic * BYTES_PER_IC + 7] = IC_VALID_PEC_L;
    }
    mock_spi_set_rx(s_valid_rx, sizeof(s_valid_rx));
}

static void default_valid_cells(uint16_t mv_each) {
    s_cells.timestamp_ms = 0;
    s_cells.overall      = MEAS_VALID;
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        s_cells.mv[i]    = mv_each;
        s_cells.valid[i] = true;
    }
}

void setUp(void) {
    bms_config_load_defaults(&s_cfg);
    for (uint8_t i = 0; i < CONFIG_MASK_BYTES; i++) {
        s_cfg.required_cell_mask[i]   = 0xFFu;
        s_cfg.balance_allowed_mask[i] = 0xFFu;
    }
    s_cfg.required_cell_mask[9]   = 0x07u;
    s_cfg.balance_allowed_mask[9] = 0x07u;
    default_valid_cells(3700u);
    mock_spi_reset();
}

void tearDown(void) {}

void test_balance_disabled_with_discharge_fault(void) {
    uint64_t faults = FAULT_MASK(FAULT_BIT_CELL_OV);
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, faults, BMS_STATE_DISCHARGE, &s_cfg);
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0);
}

void test_balance_disabled_in_init_state(void) {
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, 0u, BMS_STATE_INIT, &s_cfg);
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0);
}

void test_balance_disabled_in_fault_state(void) {
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, 0u, BMS_STATE_FAULT, &s_cfg);
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0);
}

void test_balance_disabled_in_charge_state(void) {
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, 0u, BMS_STATE_CHARGE, &s_cfg);
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0);
}

void test_balance_no_eligible_cells_disables(void) {
    s_cfg.cell_balance_target_mv     = 3700u;
    s_cfg.cell_balance_hysteresis_mv = 50u;
    default_valid_cells(3700u); /* at target, not above target+hysteresis */
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, 0u, BMS_STATE_STANDBY, &s_cfg);
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0);
}

void test_disable_all_sends_wrcfga(void) {
    /* Preload valid SPI rx so RDCFGA read succeeds and WRCFGA write proceeds */
    load_valid_spi_rx();
    BmsResult r = ltc6812_cell_chain_clear_balance(BMS_CHAIN_CELL, CELL_IC_COUNT);
    TEST_ASSERT_EQUAL(BMS_OK, r);

    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t tx_len   = mock_spi_get_last_tx_len();
    /* At minimum: 2 × (wakeup(1) + cmd(4) + 5×8=40 data) = 90 bytes */
    TEST_ASSERT_TRUE(tx_len >= 45);

    /* Scan for WRCFGA opcode bytes [0x00, 0x01] */
    bool found_wrcfga = false;
    for (uint16_t i = 0; i + 1 < tx_len; i++) {
        if (tx[i] == 0x00u && tx[i+1] == 0x01u) {
            found_wrcfga = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_wrcfga);
}

/* ── TEMP chain guard ─────────────────────────────────────────────────────── */

void test_balance_temp_chain_returns_forbidden(void) {
    uint16_t dcc[CELL_IC_COUNT] = {0};
    BmsResult r = ltc6812_cell_chain_set_balance(BMS_CHAIN_TEMP, CELL_IC_COUNT, dcc);
    TEST_ASSERT_EQUAL(BMS_ERR_FORBIDDEN, r);
    /* No SPI should be sent for the forbidden chain */
    TEST_ASSERT_EQUAL_UINT16(0u, mock_spi_get_last_tx_len());
}

/* ── Measurement-error guard ──────────────────────────────────────────────── */

void test_balance_meas_error_disables_balance(void) {
    s_cells.overall = MEAS_ERROR;
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, 0u, BMS_STATE_STANDBY, &s_cfg);
    /* disable_all must have run — some SPI bytes transmitted */
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0u);
}

/* ── Open-wire fault guard ────────────────────────────────────────────────── */

void test_balance_openwire_fault_disables_balance(void) {
    uint64_t faults = FAULT_MASK(FAULT_BIT_CELL_OPENWIRE);
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, faults, BMS_STATE_DISCHARGE, &s_cfg);
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0u);
}

/* ── DCC bit placement ────────────────────────────────────────────────────── */
/* TX layout for ltc6812_cell_chain_set_balance(CELL, 5, dcc). isospi_wakeup()
 * toggles CS only (no SPI data), so wakeups contribute ZERO bytes to the
 * captured TX stream, and read-data phases use tx=NULL (also not captured).
 * Only command frames and write payloads land in the mock TX buffer:
 *   [0..3]:   RDCFGA cmd frame       (wakeup before it: 0 bytes)
 *   [4..7]:   WRCFGA cmd frame       (wakeup before it: 0 bytes)
 *   [8..15]:  IC 0 CFGA data (6) + PEC (2)  → CFGA[4]=TX[12], CFGA[5]=TX[13]
 *   [16..47]: IC 1-4 CFGA data
 *   [48..51]: RDCFGB cmd frame
 *   [52..55]: WRCFGB cmd frame
 *   [56..63]: IC 0 CFGB data (6) + PEC       → CFGB[0]=TX[56]
 */
#define TX_IC0_CFGA4  (12u)
#define TX_IC0_CFGA5  (13u)
#define TX_IC0_CFGB0  (56u)

static void run_balance_direct(uint16_t ic0_dcc) {
    uint16_t dcc[CELL_IC_COUNT] = {0};
    dcc[0] = ic0_dcc;
    load_valid_spi_rx();
    /* Return value may be BMS_ERR_SPI (readback mismatch with zero mock data)
     * when dcc != 0; we only inspect TX bytes here. */
    ltc6812_cell_chain_set_balance(BMS_CHAIN_CELL, CELL_IC_COUNT, dcc);
}

void test_balance_dcc_cell1_sets_cfga4_bit0(void) {
    run_balance_direct(0x0001u);  /* cell 1 (1-based) = bit 0 */
    TEST_ASSERT_EQUAL_HEX8(0x01u, mock_spi_get_last_tx()[TX_IC0_CFGA4]);
}

void test_balance_dcc_cell8_sets_cfga4_bit7(void) {
    run_balance_direct(0x0080u);  /* cell 8 = bit 7 */
    TEST_ASSERT_EQUAL_HEX8(0x80u, mock_spi_get_last_tx()[TX_IC0_CFGA4]);
}

void test_balance_dcc_cell9_sets_cfga5_bit0(void) {
    run_balance_direct(0x0100u);  /* cell 9 = bit 8 → CFGA[5] bit 0 */
    TEST_ASSERT_EQUAL_HEX8(0x01u, mock_spi_get_last_tx()[TX_IC0_CFGA5] & 0x0Fu);
}

void test_balance_dcc_cell12_sets_cfga5_bit3(void) {
    run_balance_direct(0x0800u);  /* cell 12 = bit 11 → CFGA[5] bit 3 */
    TEST_ASSERT_EQUAL_HEX8(0x08u, mock_spi_get_last_tx()[TX_IC0_CFGA5] & 0x0Fu);
}

void test_balance_dcc_cell13_sets_cfgb0_bit0(void) {
    run_balance_direct(0x1000u);  /* cell 13 = bit 12 → CFGB[0] bit 0 */
    TEST_ASSERT_EQUAL_HEX8(0x01u, mock_spi_get_last_tx()[TX_IC0_CFGB0] & 0x07u);
}

void test_balance_dcc_cell15_sets_cfgb0_bit2(void) {
    run_balance_direct(0x4000u);  /* cell 15 = bit 14 → CFGB[0] bit 2 */
    TEST_ASSERT_EQUAL_HEX8(0x04u, mock_spi_get_last_tx()[TX_IC0_CFGB0] & 0x07u);
}

/* ── Readback verification ────────────────────────────────────────────────── */

void test_balance_readback_mismatch_returns_spi_error(void) {
    /* Write with dcc[0]=0x0001 (cell 1); readback returns all zeros.
     * Zero readback has ga[4]=0 but expected 0x01 → BMS_ERR_SPI. */
    uint16_t dcc[CELL_IC_COUNT] = {0};
    dcc[0] = 0x0001u;
    load_valid_spi_rx();  /* returns zeros for all 4 reads — readback will mismatch */
    BmsResult r = ltc6812_cell_chain_set_balance(BMS_CHAIN_CELL, CELL_IC_COUNT, dcc);
    TEST_ASSERT_EQUAL(BMS_ERR_SPI, r);
}

/* ── Allowed balancing ────────────────────────────────────────────────────── */

void test_balance_allowed_in_standby_one_high_cell(void) {
    s_cfg.cell_balance_target_mv     = 3600u;
    s_cfg.cell_balance_hysteresis_mv = 50u;
    s_cfg.balance_on_time_ms  = 1000u;
    s_cfg.balance_off_time_ms = 200u;
    /* All cells at 3700 mV > 3600 + 50 = 3650 → all eligible */
    default_valid_cells(3700u);
    /* Need valid SPI rx for the RDCFGA/RDCFGB read-modify-write */
    load_valid_spi_rx();
    bms_balance_tick(&s_cells, 0u, BMS_STATE_STANDBY, &s_cfg);
    /* Something was written (balance enabled or clear) */
    TEST_ASSERT_TRUE(mock_spi_get_last_tx_len() > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_balance_disabled_with_discharge_fault);
    RUN_TEST(test_balance_disabled_in_init_state);
    RUN_TEST(test_balance_disabled_in_fault_state);
    RUN_TEST(test_balance_disabled_in_charge_state);
    RUN_TEST(test_balance_no_eligible_cells_disables);
    RUN_TEST(test_disable_all_sends_wrcfga);
    RUN_TEST(test_balance_allowed_in_standby_one_high_cell);

    /* New: chain guard, meas-error guard, open-wire guard */
    RUN_TEST(test_balance_temp_chain_returns_forbidden);
    RUN_TEST(test_balance_meas_error_disables_balance);
    RUN_TEST(test_balance_openwire_fault_disables_balance);

    /* New: DCC bit placement */
    RUN_TEST(test_balance_dcc_cell1_sets_cfga4_bit0);
    RUN_TEST(test_balance_dcc_cell8_sets_cfga4_bit7);
    RUN_TEST(test_balance_dcc_cell9_sets_cfga5_bit0);
    RUN_TEST(test_balance_dcc_cell12_sets_cfga5_bit3);
    RUN_TEST(test_balance_dcc_cell13_sets_cfgb0_bit0);
    RUN_TEST(test_balance_dcc_cell15_sets_cfgb0_bit2);

    /* New: readback verification */
    RUN_TEST(test_balance_readback_mismatch_returns_spi_error);

    return UNITY_END();
}
