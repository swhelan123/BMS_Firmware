/* test_openwire.c — ltc6812_run_open_wire unit tests.
 *
 * Algorithm under test = LTC6812-1 datasheet Rev B pp.30-31:
 *   PUP pass (10× ADOW PUP=1) → read CELLPU; PDN pass (10× ADOW PUP=0) →
 *   read CELLPD; C(n) open when CELLΔ(n+1) = CELLPU(n+1)−CELLPD(n+1) < −400 mV
 *   (n = 1..14); C(0) open when CELLPU(1)=0; C(15) open when CELLPD(15)=0.
 *   An open pin flags BOTH adjacent cells in the result mask.
 *
 * RX buffer layout (ADOW commands are TX-only; ADC completion is a fixed
 * delay, so no poll bytes appear in the RX stream):
 *   [0..199]   : PUP cell data (5 groups × 5 ICs × 8 bytes = 200 bytes)
 *   [200..399] : PDN cell data (200 bytes)
 *
 * Safety invariant tested: ltc6812_run_open_wire never touches WRCFGA/WRCFGB.
 */
#include "unity.h"
#include "ltc6812.h"
#include "isospi.h"
#include "mock_board_spi.h"
#include "board_clock.h"
#include "bms_constants.h"
#include "bms_types.h"
#include <string.h>

/* ── RX buffer layout constants ─────────────────────────────────────────────── */
#define PUP_DATA_OFFSET    (0u)
#define PDN_DATA_OFFSET    (200u)
#define OPENWIRE_RX_SIZE   (400u)
#define BYTES_PER_IC_GRP   (8u)    /* 6 data + 2 PEC */
#define GROUPS_PER_CHAIN   (5u)

/* Healthy uniform cell reading: 3500 mV (raw = 100 µV units). */
#define HEALTHY_RAW        (35000u)

static uint8_t s_rx[OPENWIRE_RX_SIZE];

/* Build one 8-byte IC group (6 data bytes + 2 PEC) from three 16-bit raw values */
static void build_ic_group(uint8_t *buf, uint16_t r0, uint16_t r1, uint16_t r2) {
    buf[0] = (uint8_t)(r0);       buf[1] = (uint8_t)(r0 >> 8u);
    buf[2] = (uint8_t)(r1);       buf[3] = (uint8_t)(r1 >> 8u);
    buf[4] = (uint8_t)(r2);       buf[5] = (uint8_t)(r2 >> 8u);
    uint16_t pec = isospi_pec15(buf, 6u);
    buf[6] = (uint8_t)(pec >> 8u);
    buf[7] = (uint8_t)(pec & 0xFFu);
}

/* Fill a 200-byte cell data region with a uniform raw value for all cells */
static void fill_uniform(uint8_t *dst, uint16_t raw) {
    uint8_t grp[BYTES_PER_IC_GRP];
    build_ic_group(grp, raw, raw, raw);
    for (uint8_t g = 0; g < GROUPS_PER_CHAIN; g++) {
        for (uint8_t ic = 0; ic < CELL_IC_COUNT; ic++) {
            memcpy(dst + g * CELL_IC_COUNT * BYTES_PER_IC_GRP
                       + ic  * BYTES_PER_IC_GRP, grp, BYTES_PER_IC_GRP);
        }
    }
}

/* Override cell data for one specific cell within an IC in a data region.
 * cell_within_ic is 0-based (0..CELLS_PER_IC-1). */
static void override_cell(uint8_t *dst, uint8_t ic_idx,
                           uint8_t cell_within_ic, uint16_t raw) {
    uint8_t grp_idx      = cell_within_ic / 3u;
    uint8_t cell_in_grp  = cell_within_ic % 3u;
    uint8_t *base = dst + grp_idx * CELL_IC_COUNT * BYTES_PER_IC_GRP
                        + ic_idx  * BYTES_PER_IC_GRP;
    base[cell_in_grp * 2u]     = (uint8_t)(raw);
    base[cell_in_grp * 2u + 1u] = (uint8_t)(raw >> 8u);
    /* Recompute PEC for the modified group */
    uint16_t pec = isospi_pec15(base, 6u);
    base[6] = (uint8_t)(pec >> 8u);
    base[7] = (uint8_t)(pec & 0xFFu);
}

/* Load RX buffer with uniform PUP/PDN cell data */
static void load_rx_uniform(uint16_t pup_raw, uint16_t pdn_raw) {
    memset(s_rx, 0, sizeof(s_rx));
    fill_uniform(s_rx + PUP_DATA_OFFSET, pup_raw);
    fill_uniform(s_rx + PDN_DATA_OFFSET, pdn_raw);
    mock_spi_set_rx(s_rx, sizeof(s_rx));
}

/* ── setUp / tearDown ─────────────────────────────────────────────────────── */

void setUp(void) {
    mock_spi_reset();
    board_clock_init();
}

void tearDown(void) {}

/* ── Tests ────────────────────────────────────────────────────────────────── */

void test_openwire_temp_chain_returns_invalid_arg(void) {
    bool ow[TOTAL_CELL_COUNT] = {false};
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_TEMP, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_ERR_INVALID_ARG, r);
}

void test_openwire_pec_fail_returns_pec_error(void) {
    /* No valid RX data; mock returns 0xFF past end of buffer →
     * received PEC = 0xFFFF != computed → ERR_PEC on the PUP pass reads. */
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                                         (bool[TOTAL_CELL_COUNT]){false});
    TEST_ASSERT_EQUAL(BMS_ERR_PEC, r);
}

void test_openwire_no_wire_returns_ok(void) {
    /* PUP = PDN = healthy → all deltas 0, no endpoint zeros */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
}

void test_openwire_no_wire_all_clear(void) {
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_adow_cmds_repeated_10x_each_direction(void) {
    /* Datasheet Table 14: 100 nF C-pin caps → 10 ADOW commands per direction.
     * PUP opcode 0x0368 → bytes [0x03, 0x68]; PDN 0x0328 → [0x03, 0x28]. */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    uint16_t n_pup = 0, n_pdn = 0;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x03u && tx[i + 1u] == 0x68u) { n_pup++; }
        if (tx[i] == 0x03u && tx[i + 1u] == 0x28u) { n_pdn++; }
    }
    TEST_ASSERT_EQUAL_UINT16(10u, n_pup);
    TEST_ASSERT_EQUAL_UINT16(10u, n_pdn);
}

void test_openwire_pup_pass_before_pdn_pass(void) {
    /* Datasheet step 1 = PUP, step 2 = PDN */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    int32_t pup_pos = -1, pdn_pos = -1;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x03u && tx[i + 1u] == 0x68u && pup_pos < 0) {
            pup_pos = (int32_t)i;
        }
        if (tx[i] == 0x03u && tx[i + 1u] == 0x28u && pdn_pos < 0) {
            pdn_pos = (int32_t)i;
        }
    }
    TEST_ASSERT_TRUE(pup_pos >= 0);
    TEST_ASSERT_TRUE(pdn_pos > pup_pos);
}

void test_openwire_intermediate_pin_open_flags_both_cells(void) {
    /* Pin C(1) of IC 0 open: CELLΔ(2) < −400 mV.
     * Cell 2 (0-based idx 1): PUP = 3000 mV, PDN = 3500 mV → Δ = −500 mV.
     * C(1) sits between cells 1 and 2 → flags idx 0 and idx 1. */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    override_cell(s_rx + PUP_DATA_OFFSET, 0u, 1u, 30000u);
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    TEST_ASSERT_TRUE(ow[0]);
    TEST_ASSERT_TRUE(ow[1]);
    for (uint8_t i = 2u; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_delta_at_minus_400_not_detected(void) {
    /* Δ = exactly −400 mV: NOT strictly < −400 → not open.
     * Cell 2 idx 1: PUP = 3100 mV, PDN = 3500 mV. */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    override_cell(s_rx + PUP_DATA_OFFSET, 0u, 1u, 31000u);
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_positive_delta_not_detected(void) {
    /* Regression vs the old reversed-sign bug: a large POSITIVE delta is
     * not an open wire per the datasheet. PUP = 5001 mV, PDN = 0... but
     * PDN=0 on cell 15 means C(15); use mid-chain cell 5 (idx 4) instead:
     * PUP = 4000 mV, PDN = 3000 mV → Δ = +1000 → must NOT flag. */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    override_cell(s_rx + PUP_DATA_OFFSET, 0u, 4u, 40000u);
    override_cell(s_rx + PDN_DATA_OFFSET, 0u, 4u, 30000u);
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_bottom_endpoint_c0(void) {
    /* C(0) open: CELLPU(1) = 0 on IC 0 → flags cell idx 0 only. */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    override_cell(s_rx + PUP_DATA_OFFSET, 0u, 0u, 0u);
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    TEST_ASSERT_TRUE(ow[0]);
    for (uint8_t i = 1u; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_top_endpoint_c15(void) {
    /* C(15) open: CELLPD(15) = 0 on IC 1 → flags that IC's cell idx 14. */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    override_cell(s_rx + PDN_DATA_OFFSET, 1u, (uint8_t)(CELLS_PER_IC - 1u), 0u);
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT];
    memset(ow, 0, sizeof(ow));
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    uint8_t top_idx = (uint8_t)(1u * CELLS_PER_IC + (CELLS_PER_IC - 1u));
    TEST_ASSERT_TRUE(ow[top_idx]);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        if (i == top_idx) { continue; }
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_no_wrcfga_in_tx(void) {
    /* Open-wire detection must never touch balance config registers */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    /* WRCFGA opcode = 0x0001 → bytes [0x00, 0x01] */
    bool found_wrcfga = false;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x00u && tx[i + 1u] == 0x01u) { found_wrcfga = true; break; }
    }
    TEST_ASSERT_FALSE(found_wrcfga);
}

void test_openwire_no_wrcfgb_in_tx(void) {
    /* WRCFGB opcode = 0x0024 → bytes [0x00, 0x24] must not appear */
    load_rx_uniform(HEALTHY_RAW, HEALTHY_RAW);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    bool found_wrcfgb = false;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x00u && tx[i + 1u] == 0x24u) { found_wrcfgb = true; break; }
    }
    TEST_ASSERT_FALSE(found_wrcfgb);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_openwire_temp_chain_returns_invalid_arg);
    RUN_TEST(test_openwire_pec_fail_returns_pec_error);
    RUN_TEST(test_openwire_no_wire_returns_ok);
    RUN_TEST(test_openwire_no_wire_all_clear);
    RUN_TEST(test_openwire_adow_cmds_repeated_10x_each_direction);
    RUN_TEST(test_openwire_pup_pass_before_pdn_pass);
    RUN_TEST(test_openwire_intermediate_pin_open_flags_both_cells);
    RUN_TEST(test_openwire_delta_at_minus_400_not_detected);
    RUN_TEST(test_openwire_positive_delta_not_detected);
    RUN_TEST(test_openwire_bottom_endpoint_c0);
    RUN_TEST(test_openwire_top_endpoint_c15);
    RUN_TEST(test_openwire_no_wrcfga_in_tx);
    RUN_TEST(test_openwire_no_wrcfgb_in_tx);
    return UNITY_END();
}
