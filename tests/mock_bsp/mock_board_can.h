/* mock_board_can.h — mock bxCAN driver for host-compiled tests. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Queue one frame for the next board_can_receive() call to return.
 * Only one frame may be queued at a time (tests drain it before queuing another). */
void mock_can_inject_rx(uint32_t id, bool is_extended, const uint8_t *data, uint8_t len);

/* Last frame passed to board_can_send() or board_can_send_ext(). */
bool     mock_can_get_last_tx(uint32_t *id, bool *is_extended, uint8_t *data, uint8_t *len);

/* Number of times board_can_send()/send_ext() has been called since reset. */
uint32_t mock_can_get_tx_count(void);

/* Current mode as last set by board_can_set_charge_mode() (false=DRIVE, true=CHARGE). */
bool mock_can_get_charge_mode(void);

void mock_can_reset(void);
