/* ltc6812.c — LTC6812 device driver.
 *
 * Chain safety enforced here:
 *   - No DCC writes to TEMP chain (ltc6812_cell_chain_set_balance refuses TEMP)
 *   - S-output control only on TEMP chain (temp_chain_set_sensor_bias refuses CELL)
 *
 * CFGA/CFGB bit layouts (from LTC6812 datasheet Table 43/44):
 *   CFGA[0]: ADCOPT(6), REFON(5), GPIO5:1(4:0)
 *   CFGA[1]: VUV[7:0]
 *   CFGA[2]: VOV[3:0] | VUV[11:8]
 *   CFGA[3]: VOV[11:4]
 *   CFGA[4]: DCC[8:1]    (discharge cell 1 = bit 0, cell 8 = bit 7)
 *   CFGA[5]: DCTO[7:4] | DCC[12:9]
 *   CFGB[0]: MUTE(7) | DTMEN(4) | DCC15:13 (bits 2:0) | DCC0 (bit 3 = cell 0 discharge, unused on LTC6812)
 *   CFGB[0] bits [2:0] = DCC[15:13]
 */
#include "ltc6812.h"
#include "isospi.h"
#include "board_clock.h"
#include <string.h>

/* ── PLADC polling ───────────────────────────────────────────────────────── */
/* PLADC command: SDO goes HIGH when conversion is complete.
 * We poll by sending PLADC and clocking out one dummy byte, reading SDO.
 * Max conversion time at 7 kHz mode (MD=01): ~1.1 ms per device for ADCV.
 * Conservative timeout: 15 ms for 5-device chain with margin. */
#define ADC_POLL_TIMEOUT_MS  (15u)
#define ADC_MIN_WAIT_MS      (2u)   /* minimum before polling to avoid hammering */

static BmsResult poll_adc_done(BmsChain chain) {
    /* Wait a minimum time before starting to poll */
    board_clock_delay_ms(ADC_MIN_WAIT_MS);

    uint32_t start = board_clock_get_ms();
    uint8_t poll_rx[1] = {0};

    while ((board_clock_get_ms() - start) < ADC_POLL_TIMEOUT_MS) {
        /* Send PLADC broadcast: 4-byte command, then read 1 byte.
         * SDO = 0xFF when conversion complete, 0x00 when busy. */
        isospi_cmd_broadcast(chain, LTC_CMD_PLADC);
        /* Read one byte after the command to sample SDO */
        isospi_read_byte_after_cmd(chain, poll_rx);
        if (poll_rx[0] == 0xFFu) { return BMS_OK; }
        board_clock_delay_ms(1u);
    }
    return BMS_ERR_TIMEOUT;
}

/* ── Encode/decode cell voltage groups ───────────────────────────────────── */
/* LTC6812: 16-bit LE, 100 µV/LSB → mV = raw / 10 (integer truncation) */
static uint16_t decode_cell_mv(const uint8_t *bytes) {
    uint16_t raw = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return (uint16_t)(raw / 10u);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
BmsResult ltc6812_init_chain(BmsChain chain, uint8_t num_ics) {
    isospi_wakeup(chain);

    uint8_t cfga_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfga_data, 0, sizeof(cfga_data));
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        uint8_t *g = &cfga_data[ic * LTC6812_REG_GROUP_BYTES];
        g[0] = 0xF8u; /* GPIO1-5 high (not driven), REFON=0, ADCOPT=0 */
        g[1] = 0x00u; /* VUV = 0 (no UV hardware threshold) */
        g[2] = 0x00u;
        g[3] = 0x00u; /* VOV = 0 (no OV hardware threshold — SW evaluates) */
        g[4] = 0x00u; /* DCC8:1 = 0 (no balancing) */
        g[5] = 0x00u; /* DCTO=0, DCC12:9 = 0 */
    }
    BmsResult r = isospi_write_all(chain, LTC_CMD_WRCFGA, cfga_data, num_ics);
    if (r != BMS_OK) { return r; }

    /* Clear CFGB (DCC15:13 = 0, sensor bias cleared) */
    uint8_t cfgb_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfgb_data, 0, sizeof(cfgb_data));
    return isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb_data, num_ics);
}

/* ── Read cells (CELL or TEMP chain, ADCV + RDCVA–RDCVE) ────────────────── */
BmsResult ltc6812_read_cells(BmsChain chain, uint8_t num_ics,
                              uint16_t raw_mv[CELL_IC_COUNT][CELLS_PER_IC],
                              bool pec_ok[CELL_IC_COUNT]) {
    isospi_wakeup(chain);

    BmsResult r = isospi_cmd_broadcast(chain, LTC_CMD_ADCV);
    if (r != BMS_OK) { return r; }

    r = poll_adc_done(chain);
    if (r != BMS_OK) { return r; }

    static const uint16_t cv_cmds[5] = {
        LTC_CMD_RDCVA, LTC_CMD_RDCVB, LTC_CMD_RDCVC, LTC_CMD_RDCVD, LTC_CMD_RDCVE
    };

    bool pec_grp[ISOSPI_MAX_ICS];
    uint8_t grp_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];

    for (uint8_t ic = 0; ic < num_ics; ic++) { pec_ok[ic] = true; }

    bool any_pec_err = false;
    for (uint8_t grp = 0; grp < 5u; grp++) {
        r = isospi_read_all(chain, cv_cmds[grp], grp_data, num_ics, pec_grp);
        for (uint8_t ic = 0; ic < num_ics; ic++) {
            if (!pec_grp[ic]) { pec_ok[ic] = false; any_pec_err = true; }
            const uint8_t *g = &grp_data[ic * LTC6812_REG_GROUP_BYTES];
            uint8_t base_cell = grp * 3u;
            for (uint8_t c = 0; c < 3u && (base_cell + c) < CELLS_PER_IC; c++) {
                raw_mv[ic][base_cell + c] = decode_cell_mv(&g[c * 2u]);
            }
        }
    }

    return any_pec_err ? BMS_ERR_PEC : BMS_OK;
}

/* ── TEMP chain S-output (sensor bias) ────────────────────────────────────── */
BmsResult ltc6812_temp_chain_set_sensor_bias(BmsChain chain, uint8_t num_ics,
                                              uint16_t s_mask_per_ic) {
    if (chain != BMS_CHAIN_TEMP) { return BMS_ERR_FORBIDDEN; }

    /* S-outputs are controlled via CFGB bytes.
     * CFGB[0] bits [2:0] = S3:S1 (for LTC6812, S-outputs numbered from device gpio)
     * OQ-TMP: verify exact CFGB Sx bit mapping from LTC6812 datasheet Table 44. */
    uint8_t cfgb_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfgb_data, 0, sizeof(cfgb_data));
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        uint8_t *g = &cfgb_data[ic * LTC6812_REG_GROUP_BYTES];
        /* Lower byte: S8:S1 outputs (bits 7:0) */
        g[0] = (uint8_t)(s_mask_per_ic & 0xFFu);
        /* Upper nybble of byte 1: S15:S9 in bits [6:0] of byte 1 */
        g[1] = (uint8_t)((s_mask_per_ic >> 8u) & 0x7Fu);
    }
    return isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb_data, num_ics);
}

BmsResult ltc6812_temp_chain_clear_s_outputs(BmsChain chain, uint8_t num_ics) {
    if (chain != BMS_CHAIN_TEMP) { return BMS_ERR_FORBIDDEN; }
    uint8_t cfgb_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfgb_data, 0, sizeof(cfgb_data));
    return isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb_data, num_ics);
}

/* ── CELL chain balance control ────────────────────────────────────────────── */
/* DCC bit mapping per LTC6812 datasheet:
 *   CFGA[4] bits[7:0] = DCC8:DCC1 (cell 8 = bit7, cell 1 = bit0)
 *   CFGA[5] bits[3:0] = DCC12:DCC9
 *   CFGB[0] bits[2:0] = DCC15:DCC13
 * dcc_mask[ic] bit N = discharge cell N+1 (0-based). */
BmsResult ltc6812_cell_chain_set_balance(BmsChain chain, uint8_t num_ics,
                                          const uint16_t dcc_mask[CELL_IC_COUNT]) {
    if (chain != BMS_CHAIN_CELL) {
        /* SAFETY ENFORCEMENT: DCC write to TEMP chain is a fatal programming error */
        return BMS_ERR_FORBIDDEN;
    }

    /* Read-modify-write CFGA to preserve OV/UV and GPIO settings */
    uint8_t cfga_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    bool pec_ok[ISOSPI_MAX_ICS];
    BmsResult r = isospi_read_all(chain, LTC_CMD_RDCFGA, cfga_data, num_ics, pec_ok);
    if (r != BMS_OK) { return r; }

    for (uint8_t ic = 0; ic < num_ics; ic++) {
        uint8_t *g = &cfga_data[ic * LTC6812_REG_GROUP_BYTES];
        uint16_t dcc = dcc_mask[ic] & 0x7FFFu; /* 15 cells */

        /* CFGA[4]: DCC8:1 */
        g[4] = (uint8_t)(dcc & 0xFFu);
        /* CFGA[5]: preserve DCTO[7:4], set DCC12:9 in bits[3:0] */
        g[5] = (uint8_t)((g[5] & 0xF0u) | ((dcc >> 8u) & 0x0Fu));
    }
    r = isospi_write_all(chain, LTC_CMD_WRCFGA, cfga_data, num_ics);
    if (r != BMS_OK) { return r; }

    /* Write CFGB for DCC15:13 */
    uint8_t cfgb_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    bool pec_ok_b[ISOSPI_MAX_ICS];
    r = isospi_read_all(chain, LTC_CMD_RDCFGB, cfgb_data, num_ics, pec_ok_b);
    if (r != BMS_OK) { return r; }

    for (uint8_t ic = 0; ic < num_ics; ic++) {
        uint8_t *g = &cfgb_data[ic * LTC6812_REG_GROUP_BYTES];
        uint8_t dcc_hi = (uint8_t)((dcc_mask[ic] >> 12u) & 0x07u); /* DCC15:13 */
        /* CFGB[0] bits[2:0] = DCC15:13; preserve MUTE/DTMEN bits */
        g[0] = (uint8_t)((g[0] & 0xF8u) | dcc_hi);
    }
    r = isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb_data, num_ics);
    if (r != BMS_OK) { return r; }

    /* Readback verification: re-read CFGA and CFGB and check DCC bits match. */
    uint8_t rb_cfga[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    uint8_t rb_cfgb[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    bool rb_pec[ISOSPI_MAX_ICS];

    r = isospi_read_all(chain, LTC_CMD_RDCFGA, rb_cfga, num_ics, rb_pec);
    if (r != BMS_OK) { return r; }
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        if (!rb_pec[ic]) { return BMS_ERR_PEC; }
    }

    r = isospi_read_all(chain, LTC_CMD_RDCFGB, rb_cfgb, num_ics, rb_pec);
    if (r != BMS_OK) { return r; }
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        if (!rb_pec[ic]) { return BMS_ERR_PEC; }
        const uint8_t *ga = &rb_cfga[ic * LTC6812_REG_GROUP_BYTES];
        const uint8_t *gb = &rb_cfgb[ic * LTC6812_REG_GROUP_BYTES];
        uint16_t dcc = dcc_mask[ic] & 0x7FFFu;
        if (ga[4] != (uint8_t)(dcc & 0xFFu))           { return BMS_ERR_SPI; }
        if ((ga[5] & 0x0Fu) != (uint8_t)((dcc >> 8u) & 0x0Fu)) { return BMS_ERR_SPI; }
        if ((gb[0] & 0x07u) != (uint8_t)((dcc >> 12u) & 0x07u)) { return BMS_ERR_SPI; }
    }
    return BMS_OK;
}

BmsResult ltc6812_cell_chain_clear_balance(BmsChain chain, uint8_t num_ics) {
    uint16_t zero[CELL_IC_COUNT] = {0};
    return ltc6812_cell_chain_set_balance(chain, num_ics, zero);
}

/* ── Open-wire detection ───────────────────────────────────────────────────── */
/* ADOW command: pull-up (PUP=1) or pull-down (PUP=0) mode.
 * Sequence: run ADOW PUP=0, read all cells → save; run ADOW PUP=1, read all cells → save.
 * Open wire on cell N: |V_PUP[N] - V_PDN[N]| large (cell disconnected) or
 *   V_PDN[N] = 0 (wire between N-1 and N broken from negative side). */
BmsResult ltc6812_run_open_wire(BmsChain chain, uint8_t num_ics,
                                 bool open_wire_detected[TOTAL_CELL_COUNT]) {
    if (chain != BMS_CHAIN_CELL) { return BMS_ERR_INVALID_ARG; }

    isospi_wakeup(chain);

    uint16_t v_pup[CELL_IC_COUNT][CELLS_PER_IC];
    uint16_t v_pdn[CELL_IC_COUNT][CELLS_PER_IC];
    bool pec_ok[CELL_IC_COUNT];

    /* Pull-down pass (PUP=0): ADOW cmd with PUP=0 */
    BmsResult r = isospi_cmd_broadcast(chain, LTC_CMD_ADOW_PDN);
    if (r != BMS_OK) { return r; }
    r = poll_adc_done(chain);
    if (r != BMS_OK) { return r; }

    /* Read RDCVA-RDCVE for PDN result */
    static const uint16_t cv_cmds[5] = {
        LTC_CMD_RDCVA, LTC_CMD_RDCVB, LTC_CMD_RDCVC, LTC_CMD_RDCVD, LTC_CMD_RDCVE
    };
    uint8_t grp_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    bool pec_grp[ISOSPI_MAX_ICS];

    for (uint8_t ic = 0; ic < num_ics; ic++) { pec_ok[ic] = true; }

    for (uint8_t grp = 0; grp < 5u; grp++) {
        r = isospi_read_all(chain, cv_cmds[grp], grp_data, num_ics, pec_grp);
        for (uint8_t ic = 0; ic < num_ics; ic++) {
            if (!pec_grp[ic]) { pec_ok[ic] = false; }
            const uint8_t *g = &grp_data[ic * LTC6812_REG_GROUP_BYTES];
            uint8_t base = grp * 3u;
            for (uint8_t c = 0; c < 3u && (base + c) < CELLS_PER_IC; c++) {
                v_pdn[ic][base + c] = decode_cell_mv(&g[c * 2u]);
            }
        }
    }
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        if (!pec_ok[ic]) { return BMS_ERR_PEC; }
    }

    /* Pull-up pass (PUP=1): ADOW cmd with PUP=1 */
    isospi_wakeup(chain);
    r = isospi_cmd_broadcast(chain, LTC_CMD_ADOW_PUP);
    if (r != BMS_OK) { return r; }
    r = poll_adc_done(chain);
    if (r != BMS_OK) { return r; }

    for (uint8_t ic = 0; ic < num_ics; ic++) { pec_ok[ic] = true; }

    for (uint8_t grp = 0; grp < 5u; grp++) {
        r = isospi_read_all(chain, cv_cmds[grp], grp_data, num_ics, pec_grp);
        for (uint8_t ic = 0; ic < num_ics; ic++) {
            if (!pec_grp[ic]) { pec_ok[ic] = false; }
            const uint8_t *g = &grp_data[ic * LTC6812_REG_GROUP_BYTES];
            uint8_t base = grp * 3u;
            for (uint8_t c = 0; c < 3u && (base + c) < CELLS_PER_IC; c++) {
                v_pup[ic][base + c] = decode_cell_mv(&g[c * 2u]);
            }
        }
    }
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        if (!pec_ok[ic]) { return BMS_ERR_PEC; }
    }

    /* Evaluate open-wire condition.
     * An open wire is detected when V_PUP[n] - V_PDN[n] > threshold.
     * Conservative threshold: 400 mV (OQ-TMP: calibrate with board capacitance). */
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        for (uint8_t c = 0; c < CELLS_PER_IC; c++) {
            uint8_t idx = ic * CELLS_PER_IC + c;
            int32_t delta = (int32_t)v_pup[ic][c] - (int32_t)v_pdn[ic][c];
            open_wire_detected[idx] = (delta > 400);
        }
    }
    return BMS_OK;
}

/* ── Bring-up probe (read CFGA, no conversion, no writes) ───────────────────── */
BmsResult ltc6812_probe_chain(BmsChain chain, uint8_t num_ics,
                               bool pec_ok[5],
                               uint8_t cfga_out[5][6]) {
    isospi_wakeup(chain);
    uint8_t raw[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    bool pec_grp[ISOSPI_MAX_ICS];
    BmsResult r = isospi_read_all(chain, LTC_CMD_RDCFGA, raw, num_ics, pec_grp);
    if (r != BMS_OK) { return r; }
    bool any_fail = false;
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        pec_ok[ic] = pec_grp[ic];
        if (!pec_grp[ic]) {
            any_fail = true;
            memset(cfga_out[ic], 0, LTC6812_REG_GROUP_BYTES);
        } else {
            memcpy(cfga_out[ic], &raw[ic * LTC6812_REG_GROUP_BYTES], LTC6812_REG_GROUP_BYTES);
        }
    }
    return any_fail ? BMS_ERR_PEC : BMS_OK;
}
