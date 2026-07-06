/* board_can.h — bxCAN driver (PA11=RX, PA12=TX, ISO1050).
 *
 * Two bus modes, switched at runtime (never both active at once):
 *   DRIVE  — 500 kbit/s, TX-only telemetry to the vehicle CAN network.
 *   CHARGE — 250 kbit/s, TX+RX to the Elcon/TC charger (extended 29-bit IDs).
 *            The drive network is not present during charging (TSAC removed),
 *            so there is no bus contention between the two modes.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bms_types.h"

/* Initialise bxCAN in DRIVE mode (500 kbit/s, standard IDs, TX-only,
 * accept-all filter though nothing reads RX in this mode).
 * Call once after board_clock_init(). */
void board_can_init(void);

/* Switch between DRIVE (500 kbit/s) and CHARGE (250 kbit/s) mode.
 * Reconfigures bit timing and the RX filter; safe to call from the main
 * loop (bxCAN finishes any in-flight frame before entering init mode). */
void board_can_set_charge_mode(bool charge_mode_active);

/* Transmit one standard (11-bit ID) CAN frame, 0-8 data bytes.
 * Polls for a free mailbox; returns BMS_ERR_TIMEOUT if none free within ~1 ms.
 * Safe to call from the main loop — does not block for arbitration. */
BmsResult board_can_send(uint32_t id, const uint8_t *data, uint8_t len);

/* Transmit one extended (29-bit ID) CAN frame — used for charger comms. */
BmsResult board_can_send_ext(uint32_t id29, const uint8_t *data, uint8_t len);

/* Non-blocking receive from FIFO0. Returns true and fills *id, *is_extended,
 * data[8], and *len if a frame was pending (and releases the FIFO mailbox);
 * returns false immediately if nothing was waiting. Only meaningful in
 * CHARGE mode — DRIVE mode's filter accepts frames but nothing needs them. */
bool board_can_receive(uint32_t *id, bool *is_extended, uint8_t *data, uint8_t *len);
