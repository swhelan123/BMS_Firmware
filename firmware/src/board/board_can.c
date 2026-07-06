/* board_can.c — bxCAN driver for STM32F303VC.
 *
 * Two bit-rate modes, switched at runtime via board_can_set_charge_mode():
 *   DRIVE  — 500 kbit/s, standard (11-bit) IDs, TX-only, accept-all filter.
 *   CHARGE — 250 kbit/s, extended (29-bit) IDs, TX+RX to the Elcon/TC charger
 *            (all 1.8/3.3 kW Elcon units run CAN at 250 kbit/s; only their
 *            6.6 kW unit uses 500 kbit/s).
 * APB1 = 36 MHz. Same TS1/TS2/SJW both modes (same 77.8% sample point);
 * only BRP changes: BRP=4 → 500k, BRP=8 → 250k.
 *
 * Mode switching re-enters init mode exactly as board_can_init() does —
 * bxCAN finishes any frame already in flight before INAK is granted, so
 * this is safe to call from the main loop mid-operation.
 *
 * Filters: single filter bank 0, mask-mode.
 *   DRIVE:  32-bit, id=0/mask=0 → accept everything (nothing reads RX here).
 *   CHARGE: 32-bit, exact match on the charger's extended status ID.
 * Transmit: mailbox polling, no interrupts.
 */
#include "board_can.h"
#include "board_pins.h"
#include "bms_hal.h"
#include "bms_charger.h"  /* CHARGER_CAN_STATUS_ID */

/* ── Bit-timing register values ───────────────────────────────────────────── */
/* BTR[9:0]=BRP-1, BTR[19:16]=TS1-1, BTR[22:20]=TS2-1, BTR[25:24]=SJW-1 */
#define CAN_BTR_500KBPS  ((0u << 24) | (3u << 20) | (12u << 16) | 3u)  /* BRP=4 */
#define CAN_BTR_250KBPS  ((0u << 24) | (3u << 20) | (12u << 16) | 7u)  /* BRP=8 */

/* ── TSR mailbox-empty bits ───────────────────────────────────────────────── */
#define TSR_TME0  (1u << 26)
#define TSR_TME1  (1u << 27)
#define TSR_TME2  (1u << 28)

static bool s_charge_mode;

/* Reconfigure bit timing + filter bank 0 for the given mode. Shared by
 * board_can_init() (always starts in DRIVE) and board_can_set_charge_mode(). */
static void configure_mode(bool charge_mode) {
    CAN->MCR |= CAN_MCR_INRQ;
    while (!(CAN->MSR & CAN_MSR_INAK)) { /* wait for INAK */ }

    CAN->BTR = charge_mode ? CAN_BTR_250KBPS : CAN_BTR_500KBPS;

    CAN->FMR  |= CAN_FMR_FINIT;         /* enter filter init */
    CAN->FA1R &= ~(1u << 0);            /* deactivate bank 0 while reprogramming */
    CAN->FS1R |= (1u << 0);             /* 32-bit scale */
    CAN->FM1R &= ~(1u << 0);            /* mask mode */
    CAN->FFA1R &= ~(1u << 0);           /* assign to FIFO 0 */
    if (charge_mode) {
        /* Exact match: extended ID == CHARGER_CAN_STATUS_ID, ignore RTR.
         * FR1/FR2 mirror TIR/RIR layout: bits[31:3]=EXID, bit2=IDE, bit1=RTR.
         * Mask 0xFFFFFFFC covers EXID+IDE, leaves RTR (bit1) don't-care. */
        CAN->sFilterRegister[0].FR1 = (CHARGER_CAN_STATUS_ID << 3u) | (1u << 2u);
        CAN->sFilterRegister[0].FR2 = 0xFFFFFFFCu;
    } else {
        CAN->sFilterRegister[0].FR1 = 0u;   /* accept all */
        CAN->sFilterRegister[0].FR2 = 0u;
    }
    CAN->FA1R |= (1u << 0);             /* reactivate bank 0 */
    CAN->FMR  &= ~CAN_FMR_FINIT;        /* leave filter init */

    CAN->MCR &= ~CAN_MCR_INRQ;
    while (CAN->MSR & CAN_MSR_INAK) { /* wait for normal mode */ }

    s_charge_mode = charge_mode;
}

void board_can_init(void) {
    /* GPIO: PA11=CAN_RX, PA12=CAN_TX → AF9, push-pull, no pull */
    CAN_PORT->MODER &= ~((3u << (CAN_RX_PIN * 2u)) | (3u << (CAN_TX_PIN * 2u)));
    CAN_PORT->MODER |=  (GPIO_MODER_AF << (CAN_RX_PIN * 2u)) |
                        (GPIO_MODER_AF << (CAN_TX_PIN * 2u));
    CAN_PORT->AFR[1] &= ~(0xFu << ((CAN_RX_PIN - 8u) * 4u)) &
                         ~(0xFu << ((CAN_TX_PIN - 8u) * 4u));
    CAN_PORT->AFR[1] |=  (CAN_AF << ((CAN_RX_PIN - 8u) * 4u)) |
                         (CAN_AF << ((CAN_TX_PIN - 8u) * 4u));

    CAN->MCR |= CAN_MCR_INRQ;
    CAN->MCR &= ~CAN_MCR_SLEEP;
    while (!(CAN->MSR & CAN_MSR_INAK)) { /* wait for INAK */ }

    /* ABOM: auto bus-off recovery; TXFP: mailboxes in FIFO order */
    CAN->MCR |= CAN_MCR_ABOM | CAN_MCR_TXFP;
    CAN->MCR &= ~CAN_MCR_TTCM; /* time-triggered mode off */

    CAN->MCR &= ~CAN_MCR_INRQ;
    while (CAN->MSR & CAN_MSR_INAK) { /* wait for normal mode */ }

    configure_mode(false); /* start in DRIVE mode */
}

void board_can_set_charge_mode(bool charge_mode_active) {
    if (charge_mode_active == s_charge_mode) { return; }
    configure_mode(charge_mode_active);
}

static BmsResult send_frame(uint32_t tir_id_field, const uint8_t *data, uint8_t len) {
    if (len > 8u) { len = 8u; }

    /* Find a free mailbox (spin up to ~72000 iterations ≈ 1 ms at 72 MHz) */
    uint8_t  mbox = 0xFFu;
    uint32_t deadline = 72000u;
    while (deadline--) {
        uint32_t tsr = CAN->TSR;
        if (tsr & TSR_TME0)      { mbox = 0u; break; }
        else if (tsr & TSR_TME1) { mbox = 1u; break; }
        else if (tsr & TSR_TME2) { mbox = 2u; break; }
    }
    if (mbox == 0xFFu) { return BMS_ERR_TIMEOUT; }

    CAN->sTxMailBox[mbox].TIR  = tir_id_field;
    CAN->sTxMailBox[mbox].TDTR = (uint32_t)len;
    CAN->sTxMailBox[mbox].TDLR = ((uint32_t)data[0])        |
                                  ((uint32_t)data[1] << 8u)  |
                                  ((uint32_t)(len > 2u ? data[2] : 0u) << 16u) |
                                  ((uint32_t)(len > 3u ? data[3] : 0u) << 24u);
    CAN->sTxMailBox[mbox].TDHR = ((uint32_t)(len > 4u ? data[4] : 0u))        |
                                  ((uint32_t)(len > 5u ? data[5] : 0u) << 8u)  |
                                  ((uint32_t)(len > 6u ? data[6] : 0u) << 16u) |
                                  ((uint32_t)(len > 7u ? data[7] : 0u) << 24u);

    CAN->sTxMailBox[mbox].TIR |= CAN_TI0R_TXRQ;
    return BMS_OK;
}

BmsResult board_can_send(uint32_t id, const uint8_t *data, uint8_t len) {
    /* Standard ID: STID[10:0] in bits[31:21], RTR=0, IDE=0 */
    return send_frame(id << 21u, data, len);
}

BmsResult board_can_send_ext(uint32_t id29, const uint8_t *data, uint8_t len) {
    /* Extended ID: EXID[28:0] in bits[31:3], IDE=1 (bit2), RTR=0 */
    return send_frame((id29 << 3u) | CAN_TI0R_IDE, data, len);
}

bool board_can_receive(uint32_t *id, bool *is_extended, uint8_t *data, uint8_t *len) {
    if ((CAN->RF0R & 0x3u) == 0u) { return false; } /* FMP0: no message pending */

    uint32_t rir = CAN->sFIFOMailBox[0].RIR;
    bool ext = (rir & CAN_RI0R_IDE) != 0u;
    *is_extended = ext;
    *id  = ext ? (rir >> 3u) : (rir >> 21u);
    *len = (uint8_t)(CAN->sFIFOMailBox[0].RDTR & 0xFu);
    if (*len > 8u) { *len = 8u; }

    uint32_t lo = CAN->sFIFOMailBox[0].RDLR;
    uint32_t hi = CAN->sFIFOMailBox[0].RDHR;
    data[0] = (uint8_t)(lo);        data[1] = (uint8_t)(lo >> 8u);
    data[2] = (uint8_t)(lo >> 16u); data[3] = (uint8_t)(lo >> 24u);
    data[4] = (uint8_t)(hi);        data[5] = (uint8_t)(hi >> 8u);
    data[6] = (uint8_t)(hi >> 16u); data[7] = (uint8_t)(hi >> 24u);

    CAN->RF0R |= CAN_RF0R_RFOM0; /* release the FIFO0 output mailbox */
    return true;
}
