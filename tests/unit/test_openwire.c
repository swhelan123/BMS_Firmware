/* test_openwire.c — ltc6812_run_open_wire unit tests.
 *
 * RX buffer layout for ltc6812_run_open_wire (see ltc6812.c). ADC completion
 * is a fixed time delay (no PLADC poll — see poll_adc_done), so no poll
 * bytes appear in the RX stream:
 *   [0..199]   : PDN cell data (5 groups × 5 ICs × 8 bytes = 200 bytes)
 *   [200..399] : PUP cell data (200 bytes)
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
#define PDN_DATA_OFFSET    (0u)
#define PUP_DATA_OFFSET    (200u)
#define OPENWIRE_RX_SIZE   (400u)
#define BYTES_PER_IC_GRP   (8u)    /* 6 data + 2 PEC */
#define GROUPS_PER_CHAIN   (5u)

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

/* Load RX buffer with uniform PDN/PUP cell data */
static void load_rx_uniform(uint16_t pdn_raw, uint16_t pup_raw) {
    memset(s_rx, 0, sizeof(s_rx));
    fill_uniform(s_rx + PDN_DATA_OFFSET, pdn_raw);
    fill_uniform(s_rx + PUP_DATA_OFFSET, pup_raw);
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

void test_openwire_pec_fail_pdn_returns_pec_error(void) {
    /* No valid RX data; mock returns 0xFF past end of buffer.
     * Poll byte 0 = 0xFF → ADC done immediately.
     * RDCVA reads 40 bytes of 0xFF → received PEC = 0xFFFF != computed → ERR_PEC. */
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                                         (bool[TOTAL_CELL_COUNT]){false});
    TEST_ASSERT_EQUAL(BMS_ERR_PEC, r);
}

void test_openwire_no_wire_returns_ok(void) {
    /* PDN = PUP → delta = 0 ≤ 400 for all cells */
    load_rx_uniform(0u, 0u);
    bool ow[TOTAL_CELL_COUNT];
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
}

void test_openwire_no_wire_all_clear(void) {
    load_rx_uniform(0u, 0u);
    bool ow[TOTAL_CELL_COUNT];
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_adow_pdn_cmd_in_tx(void) {
    /* ADOW_PDN opcode = 0x0228 → bytes [0x02, 0x28] in TX stream */
    load_rx_uniform(0u, 0u);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    bool found = false;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x02u && tx[i + 1u] == 0x28u) { found = true; break; }
    }
    TEST_ASSERT_TRUE(found);
}

void test_openwire_adow_pup_cmd_in_tx(void) {
    /* ADOW_PUP opcode = 0x0268 → bytes [0x02, 0x68] in TX stream */
    load_rx_uniform(0u, 0u);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    bool found = false;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x02u && tx[i + 1u] == 0x68u) { found = true; break; }
    }
    TEST_ASSERT_TRUE(found);
}

void test_openwire_pup_cmd_after_pdn_cmd(void) {
    /* PDN pass (0x02, 0x28) must appear before PUP pass (0x02, 0x68) */
    load_rx_uniform(0u, 0u);
    ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT,
                           (bool[TOTAL_CELL_COUNT]){false});
    const uint8_t *tx = mock_spi_get_last_tx();
    uint16_t len      = mock_spi_get_last_tx_len();
    int32_t pdn_pos = -1, pup_pos = -1;
    for (uint16_t i = 0; i + 1u < len; i++) {
        if (tx[i] == 0x02u && tx[i + 1u] == 0x28u && pdn_pos < 0) {
            pdn_pos = (int32_t)i;
        }
        if (tx[i] == 0x02u && tx[i + 1u] == 0x68u && pup_pos < 0) {
            pup_pos = (int32_t)i;
        }
    }
    TEST_ASSERT_TRUE(pdn_pos >= 0);
    TEST_ASSERT_TRUE(pup_pos > pdn_pos);
}

void test_openwire_detected_cell0(void) {
    /* IC 0 cell 0: PDN = 0 mV, PUP = 5001 mV → delta = 5001 > 400 → open */
    memset(s_rx, 0, sizeof(s_rx));
    fill_uniform(s_rx + PDN_DATA_OFFSET, 0u);
    fill_uniform(s_rx + PUP_DATA_OFFSET, 0u);
    /* 5001 mV → raw = 5001 × 10 = 50010 = 0xC35A */
    override_cell(s_rx + PUP_DATA_OFFSET, 0u, 0u, 50010u);
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT] = {false};
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    TEST_ASSERT_TRUE(ow[0]);
    for (uint8_t i = 1u; i < TOTAL_CELL_COUNT; i++) {
        TEST_ASSERT_FALSE(ow[i]);
    }
}

void test_openwire_threshold_at_boundary_not_detected(void) {
    /* delta = 400 mV exactly: NOT strictly > 400 → not detected */
    /* PDN = 0, PUP = 400 mV → raw = 4000 */
    memset(s_rx, 0, sizeof(s_rx));
    fill_uniform(s_rx + PDN_DATA_OFFSET, 0u);
    fill_uniform(s_rx + PUP_DATA_OFFSET, 0u);
    override_cell(s_rx + PUP_DATA_OFFSET, 0u, 0u, 4000u);  /* 400 mV */
    mock_spi_set_rx(s_rx, sizeof(s_rx));

    bool ow[TOTAL_CELL_COUNT] = {false};
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, ow);
    TEST_ASSERT_EQUAL(BMS_OK, r);
    TEST_ASSERT_FALSE(ow[0]);
}

void test_openwire_no_wrcfga_in_tx(void) {
    /* Open-wire detection must never touch balance config registers */
    load_rx_uniform(0u, 0u);
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
    load_rx_uniform(0u, 0u);
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
    RUN_TEST(test_openwire_pec_fail_pdn_returns_pec_error);
    RUN_TEST(test_openwire_no_wire_returns_ok);
    RUN_TEST(test_openwire_no_wire_all_clear);
    RUN_TEST(test_openwire_adow_pdn_cmd_in_tx);
    RUN_TEST(test_openwire_adow_pup_cmd_in_tx);
    RUN_TEST(test_openwire_pup_cmd_after_pdn_cmd);
    RUN_TEST(test_openwire_detected_cell0);
    RUN_TEST(test_openwire_threshold_at_boundary_not_detected);
    RUN_TEST(test_openwire_no_wrcfga_in_tx);
    RUN_TEST(test_openwire_no_wrcfgb_in_tx);
    return UNITY_END();
}
