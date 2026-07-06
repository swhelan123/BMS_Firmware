/* mock_board_can.c — mock bxCAN implementation for host tests. */
#include "mock_board_can.h"
#include "board_can.h"
#include <string.h>

static bool     s_charge_mode;

static uint32_t s_tx_id;
static bool     s_tx_ext;
static uint8_t  s_tx_data[8];
static uint8_t  s_tx_len;
static uint32_t s_tx_count;
static bool     s_tx_valid;

static uint32_t s_rx_id;
static bool     s_rx_ext;
static uint8_t  s_rx_data[8];
static uint8_t  s_rx_len;
static bool     s_rx_pending;

void mock_can_reset(void) {
    s_charge_mode = false;
    s_tx_count = 0u;
    s_tx_valid = false;
    s_rx_pending = false;
    memset(s_tx_data, 0, sizeof(s_tx_data));
    memset(s_rx_data, 0, sizeof(s_rx_data));
}

void mock_can_inject_rx(uint32_t id, bool is_extended, const uint8_t *data, uint8_t len) {
    s_rx_id  = id;
    s_rx_ext = is_extended;
    if (len > 8u) { len = 8u; }
    memcpy(s_rx_data, data, len);
    s_rx_len = len;
    s_rx_pending = true;
}

bool mock_can_get_last_tx(uint32_t *id, bool *is_extended, uint8_t *data, uint8_t *len) {
    if (!s_tx_valid) { return false; }
    *id = s_tx_id;
    *is_extended = s_tx_ext;
    memcpy(data, s_tx_data, 8u);
    *len = s_tx_len;
    return true;
}

uint32_t mock_can_get_tx_count(void) { return s_tx_count; }
bool     mock_can_get_charge_mode(void) { return s_charge_mode; }

/* ── board_can.h implementation ───────────────────────────────────────────── */

void board_can_init(void) { mock_can_reset(); }

void board_can_set_charge_mode(bool charge_mode_active) {
    s_charge_mode = charge_mode_active;
}

static BmsResult record_tx(uint32_t id, bool ext, const uint8_t *data, uint8_t len) {
    s_tx_id = id; s_tx_ext = ext;
    if (len > 8u) { len = 8u; }
    memcpy(s_tx_data, data, len);
    s_tx_len = len;
    s_tx_valid = true;
    s_tx_count++;
    return BMS_OK;
}

BmsResult board_can_send(uint32_t id, const uint8_t *data, uint8_t len) {
    return record_tx(id, false, data, len);
}

BmsResult board_can_send_ext(uint32_t id29, const uint8_t *data, uint8_t len) {
    return record_tx(id29, true, data, len);
}

bool board_can_receive(uint32_t *id, bool *is_extended, uint8_t *data, uint8_t *len) {
    if (!s_rx_pending) { return false; }
    *id = s_rx_id;
    *is_extended = s_rx_ext;
    memcpy(data, s_rx_data, 8u);
    *len = s_rx_len;
    s_rx_pending = false;
    return true;
}
