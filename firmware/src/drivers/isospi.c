/* isospi.c — isoSPI transport layer for LTC6812 daisy chains. */
#include "isospi.h"
#include "board_spi.h"
#include "board_clock.h"
#include <string.h>

/* ── PEC-15 ───────────────────────────────────────────────────────────────── */
/* Polynomial 0x4599 as per LTC6812 datasheet §8.7.
 * Table driven for speed; table built on first call (done in isospi_pec15). */
#define PEC15_POLY  (0x4599u)

static uint16_t s_pec_table[256];
static bool     s_pec_table_ready;

static void pec15_build_table(void) {
    for (int i = 0; i < 256; i++) {
        uint16_t r = (uint16_t)(i << 7);
        for (int b = 0; b < 8; b++) {
            r = (r & 0x4000u) ? (uint16_t)((r << 1) ^ PEC15_POLY) : (uint16_t)(r << 1);
        }
        s_pec_table[i] = (uint16_t)(r & 0x7FFFu);
    }
    s_pec_table_ready = true;
}

uint16_t isospi_pec15(const uint8_t *data, uint8_t len) {
    if (!s_pec_table_ready) { pec15_build_table(); }
    uint16_t pec = 16u; /* init value from LTC6812 datasheet */
    for (uint8_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((pec >> 7) ^ data[i]);
        pec = (uint16_t)((pec << 8) ^ s_pec_table[idx]);
        pec &= 0x7FFFu;
    }
    return (uint16_t)(pec << 1); /* PEC LSB always 0 per datasheet */
}

/* ── Command framing ──────────────────────────────────────────────────────── */
/* Build a 4-byte command frame: CMD[1:0] + PEC[1:0] */
static void build_cmd_frame(uint16_t cmd, uint8_t out[4]) {
    out[0] = (uint8_t)(cmd >> 8);
    out[1] = (uint8_t)(cmd & 0xFFu);
    uint16_t pec = isospi_pec15(out, 2);
    out[2] = (uint8_t)(pec >> 8);
    out[3] = (uint8_t)(pec & 0xFFu);
}

/* ── Wakeup ───────────────────────────────────────────────────────────────── */
/* LTC681x isoSPI wake (matches the LTC/ADI wakeup_sleep reference).
 *
 * PREVIOUS BUG: this sent one dummy byte (~µs) then immediately deasserted
 * CS, so CS was low for microseconds — not the ~300 µs the LTC6820/LTC6812
 * need to leave IDLE. The whole chain stayed asleep and every read returned
 * NO_RESPONSE. The old 1 ms delay was AFTER CS went high (idle time), which
 * does nothing to wake the chain.
 *
 * Correct sequence, once per IC so the wake ripples the full daisy chain:
 *   CS low → hold t_WAKE (≥300 µs) → CS high → hold t_READY (≥10 µs).
 * No SPI data is clocked — the LTC6820 drives the isoSPI wake pulse purely
 * from CS going low (this matches the LTC/ADI wakeup_sleep reference).
 * Total ~ISOSPI_MAX_ICS × 310 µs ≈ 1.6 ms per wake — well within the
 * measurement-cycle budget. */
#define ISOSPI_T_WAKE_US   (300u)
#define ISOSPI_T_READY_US  (10u)

void isospi_wakeup(BmsChain chain) {
    for (uint8_t i = 0u; i < ISOSPI_MAX_ICS; i++) {
        board_spi_cs_assert(chain);
        board_clock_delay_us(ISOSPI_T_WAKE_US);
        board_spi_cs_deassert(chain);
        board_clock_delay_us(ISOSPI_T_READY_US);
    }
}

/* ── Broadcast command (no data) ─────────────────────────────────────────── */
BmsResult isospi_cmd_broadcast(BmsChain chain, uint16_t cmd) {
    uint8_t frame[4];
    build_cmd_frame(cmd, frame);
    isospi_wakeup(chain);
    board_spi_cs_assert(chain);
    board_spi_write(frame, sizeof(frame));
    board_spi_cs_deassert(chain);
    return BMS_OK;
}

/* ── Write all ICs ────────────────────────────────────────────────────────── */
BmsResult isospi_write_all(BmsChain chain, uint16_t cmd,
                           const uint8_t *data, uint8_t num_ics) {
    uint8_t frame[4];
    build_cmd_frame(cmd, frame);

    isospi_wakeup(chain);
    board_spi_cs_assert(chain);
    board_spi_write(frame, sizeof(frame));

    for (uint8_t ic = 0; ic < num_ics; ic++) {
        const uint8_t *grp = &data[ic * LTC6812_REG_GROUP_BYTES];
        uint16_t pec = isospi_pec15(grp, LTC6812_REG_GROUP_BYTES);
        uint8_t pec_bytes[2] = { (uint8_t)(pec >> 8), (uint8_t)(pec & 0xFFu) };
        board_spi_write(grp, LTC6812_REG_GROUP_BYTES);
        board_spi_write(pec_bytes, sizeof(pec_bytes));
    }

    board_spi_cs_deassert(chain);
    return BMS_OK;
}

/* ── Read one byte after a broadcast command (for PLADC polling) ─────────── */
void isospi_read_byte_after_cmd(BmsChain chain, uint8_t *out) {
    board_spi_cs_assert(chain);
    board_spi_transfer(NULL, out, 1u);
    board_spi_cs_deassert(chain);
}

/* ── Read all ICs ─────────────────────────────────────────────────────────── */
BmsResult isospi_read_all(BmsChain chain, uint16_t cmd,
                          uint8_t *data, uint8_t num_ics,
                          bool *pec_ok_per_ic) {
    uint8_t frame[4];
    build_cmd_frame(cmd, frame);

    /* Receive buffer: num_ics × (6 data + 2 PEC) */
    uint8_t rx_buf[ISOSPI_MAX_ICS * (LTC6812_REG_GROUP_BYTES + LTC6812_PEC_BYTES)];
    uint16_t rx_len = (uint16_t)(num_ics * (LTC6812_REG_GROUP_BYTES + LTC6812_PEC_BYTES));

    isospi_wakeup(chain);
    board_spi_cs_assert(chain);
    board_spi_write(frame, sizeof(frame));
    board_spi_transfer(NULL, rx_buf, rx_len);
    board_spi_cs_deassert(chain);

    /* Parse and verify each IC's register group */
    bool any_pec_err = false;
    for (uint8_t ic = 0; ic < num_ics; ic++) {
        const uint8_t *grp = &rx_buf[ic * (LTC6812_REG_GROUP_BYTES + LTC6812_PEC_BYTES)];
        uint16_t received_pec = ((uint16_t)grp[6] << 8) | grp[7];
        uint16_t computed_pec = isospi_pec15(grp, LTC6812_REG_GROUP_BYTES);
        bool ok = (received_pec == computed_pec);
        if (pec_ok_per_ic) { pec_ok_per_ic[ic] = ok; }
        if (!ok) { any_pec_err = true; }
        memcpy(&data[ic * LTC6812_REG_GROUP_BYTES], grp, LTC6812_REG_GROUP_BYTES);
    }

    return any_pec_err ? BMS_ERR_PEC : BMS_OK;
}
