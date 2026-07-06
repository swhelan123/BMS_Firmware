/* ltc6812.c — LTC6812 device driver.
 *
 * Chain safety enforced here:
 *   - No DCC writes to TEMP chain (ltc6812_cell_chain_set_balance refuses TEMP)
 *   - S-output control only on TEMP chain (temp_chain_set_sensor_bias refuses CELL)
 *
 * CFGA/CFGB bit layouts (from LTC6812 datasheet Table 43/44):
 *   CFGA[0]: GPIO5:1(7:3), REFON(2), DTEN(1, read-only), ADCOPT(0)
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
    /* PREVIOUS BUG: this PLADC-polled with CS deasserted between the command
     * and the status byte. PLADC status is only meaningful while CS stays low
     * in the SAME window; a fresh CS window samples the isoSPI idle state
     * (0xFF) and reports "done" instantly. Result observed on hardware:
     * register groups read out DURING the conversion — early groups (RDCVA-C)
     * garbage, late groups (RDCVD/E) valid because the conversion finished
     * mid-readout.
     *
     * Fix: deterministic wait for the known conversion time, like the ADI
     * reference code. ADCV all-cell at MD=7kHz takes ~2.34 ms; 4 ms covers
     * it with margin regardless of chain length (conversions run in
     * parallel on every IC). */
    (void)chain;
    board_clock_delay_ms(4u);
    return BMS_OK;
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
        g[0] = 0xFCu; /* GPIO1-5 high (not driven), REFON=1, ADCOPT=0.
                       * REFON must be set: with the reference powered down the
                       * ADC returns scattered garbage codes (observed on
                       * hardware: 0..5857mV on a uniform 3.48V segment). */
        g[1] = 0x00u; /* VUV = 0 (no UV hardware threshold) */
        g[2] = 0x00u;
        g[3] = 0x00u; /* VOV = 0 (no OV hardware threshold — SW evaluates) */
        g[4] = 0x00u; /* DCC8:1 = 0 (no balancing) */
        g[5] = 0x00u; /* DCTO=0, DCC12:9 = 0 */
    }
    BmsResult r = isospi_write_all(chain, LTC_CMD_WRCFGA, cfga_data, num_ics);
    if (r != BMS_OK) { return r; }

    /* Reference power-up time after enabling REFON (t_REFUP ≤ 4.4 ms) —
     * conversions started before this settle read garbage. */
    board_clock_delay_ms(5u);

    /* Clear CFGB (DCC15:13 = 0, sensor bias cleared) */
    uint8_t cfgb_data[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfgb_data, 0, sizeof(cfgb_data));
    return isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb_data, num_ics);
}

/* Ensure REFON is set on every IC before a conversion. The LTC6812's
 * watchdog drops it back to defaults (REFON=0) after ~2 s without comms —
 * the main loop never hits that, but spaced-out bench one-shots do. Reads
 * CFGA and only rewrites (+ t_REFUP wait) when someone actually lost it,
 * so the steady-state cost is one extra register read per cycle. */
static BmsResult ensure_refon(BmsChain chain, uint8_t num_ics) {
    uint8_t cfga[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    bool pec_ok[ISOSPI_MAX_ICS];
    BmsResult r = isospi_read_all(chain, LTC_CMD_RDCFGA, cfga, num_ics, pec_ok);
    if (r != BMS_OK) { return r; }

    bool rewrite = false;
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        uint8_t *g = &cfga[ic * LTC6812_REG_GROUP_BYTES];
        if ((g[0] & 0x04u) == 0u) {  /* REFON (CFGR0 bit 2) lost */
            g[0] |= 0x04u;
            rewrite = true;
        }
    }
    if (!rewrite) { return BMS_OK; }

    r = isospi_write_all(chain, LTC_CMD_WRCFGA, cfga, num_ics);
    if (r != BMS_OK) { return r; }
    board_clock_delay_ms(5u); /* t_REFUP */
    return BMS_OK;
}

/* ── Read cells (CELL or TEMP chain, ADCV + RDCVA–RDCVE) ────────────────── */
BmsResult ltc6812_read_cells(BmsChain chain, uint8_t num_ics,
                              uint16_t raw_mv[CELL_IC_COUNT][CELLS_PER_IC],
                              bool pec_ok[CELL_IC_COUNT]) {
    isospi_wakeup(chain);

    BmsResult r = ensure_refon(chain, num_ics);
    if (r != BMS_OK) { return r; }

    /* TEMP chain: keep discharge (sensor bias) on during the conversion
     * (DCP=1). CELL chain: pause discharge during measurement (DCP=0) so a
     * balancing cell reads its true, unloaded voltage. */
    uint16_t adcv = (chain == BMS_CHAIN_TEMP) ? LTC_CMD_ADCV_DCP : LTC_CMD_ADCV;
    r = isospi_cmd_broadcast(chain, adcv);
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

    /* The Enepaq temp sensors are biased by turning ON the discharge switch
     * S1..S15 for each channel. On the LTC6812 those switches are the cell
     * discharge outputs, controlled by DCC1..DCC15 — NOT a separate S-output
     * register. Same bit layout the balance path uses:
     *   CFGA[4]      = DCC8:1
     *   CFGA[5][3:0] = DCC12:9   (upper nibble is DCTO, keep 0)
     *   CFGB[0][2:0] = DCC15:13
     * REFON stays set in CFGA[0] so the ADC reference is powered for the
     * temp read that follows. (The previous code wrote the mask into
     * CFGB[0]/[1], which are not the DCC bits, so the sensors were never
     * biased and every channel read the raw cell-tap voltage — out of the
     * sensor window, all INVALID.) */
    uint16_t dcc = s_mask_per_ic & 0x7FFFu;

    uint8_t cfga[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfga, 0, sizeof(cfga));
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        uint8_t *g = &cfga[ic * LTC6812_REG_GROUP_BYTES];
        g[0] = 0xFCu;                              /* GPIO1-5 high, REFON=1 */
        g[4] = (uint8_t)(dcc & 0xFFu);             /* DCC8:1 */
        g[5] = (uint8_t)((dcc >> 8u) & 0x0Fu);     /* DCC12:9, DCTO=0 */
    }
    BmsResult r = isospi_write_all(chain, LTC_CMD_WRCFGA, cfga, num_ics);
    if (r != BMS_OK) { return r; }

    uint8_t cfgb[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfgb, 0, sizeof(cfgb));
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        cfgb[ic * LTC6812_REG_GROUP_BYTES] = (uint8_t)((dcc >> 12u) & 0x07u); /* DCC15:13 */
    }
    return isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb, num_ics);
}

BmsResult ltc6812_temp_chain_clear_s_outputs(BmsChain chain, uint8_t num_ics) {
    if (chain != BMS_CHAIN_TEMP) { return BMS_ERR_FORBIDDEN; }

    /* Bias switches are DCC1..DCC15, which live in CFGA[4]/[5] and CFGB[0].
     * Clear BOTH register groups — clearing only CFGB would leave DCC12:1 in
     * CFGA latched on, holding the sensor bias current. Keep REFON set. */
    uint8_t cfga[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfga, 0, sizeof(cfga));
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        cfga[ic * LTC6812_REG_GROUP_BYTES] = 0xFCu; /* GPIO1-5 high, REFON=1, DCC cleared */
    }
    BmsResult r = isospi_write_all(chain, LTC_CMD_WRCFGA, cfga, num_ics);
    if (r != BMS_OK) { return r; }

    uint8_t cfgb[ISOSPI_MAX_ICS * LTC6812_REG_GROUP_BYTES];
    memset(cfgb, 0, sizeof(cfgb));
    return isospi_write_all(chain, LTC_CMD_WRCFGB, cfgb, num_ics);
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
