/* mock_board_i2c.c — controllable I2C stub for isl28022 unit tests.
 *
 * Provides per-register-address response data so that consecutive reads to
 * different registers (e.g. VBUS at 0x02 then VSHUNT at 0x01) can return
 * different values.  Tests call mock_i2c_set_reg_data() before the SUT.
 */
#include "board_i2c.h"
#include <string.h>
#include <stdint.h>

#define MAX_REG  8u

static BmsResult s_read_result  = BMS_OK;
static BmsResult s_write_result = BMS_OK;
static uint8_t   s_reg_data[MAX_REG][2];
static uint8_t   s_last_dev_addr;
static uint8_t   s_last_read_reg;
static uint8_t   s_last_write_reg;

int mock_i2c_read_calls  = 0;
int mock_i2c_write_calls = 0;

/* ── Test-facing control API ─────────────────────────────────────────────── */

void mock_i2c_reset(void) {
    s_read_result  = BMS_OK;
    s_write_result = BMS_OK;
    mock_i2c_read_calls  = 0;
    mock_i2c_write_calls = 0;
    s_last_dev_addr  = 0;
    s_last_read_reg  = 0;
    s_last_write_reg = 0;
    memset(s_reg_data, 0, sizeof(s_reg_data));
}

void mock_i2c_set_read_result(BmsResult r)  { s_read_result  = r; }
void mock_i2c_set_write_result(BmsResult r) { s_write_result = r; }

/* Set the 2-byte response for a given register address (0–7). */
void mock_i2c_set_reg_data(uint8_t reg, uint8_t msb, uint8_t lsb) {
    if (reg < MAX_REG) {
        s_reg_data[reg][0] = msb;
        s_reg_data[reg][1] = lsb;
    }
}

uint8_t mock_i2c_last_dev_addr(void)  { return s_last_dev_addr;  }
uint8_t mock_i2c_last_read_reg(void)  { return s_last_read_reg;  }
uint8_t mock_i2c_last_write_reg(void) { return s_last_write_reg; }

/* ── board_i2c API stubs ──────────────────────────────────────────────────── */

void board_i2c_init(void) {}

BmsResult board_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t *buf, uint8_t len) {
    s_last_dev_addr = dev_addr;
    s_last_read_reg = reg_addr;
    mock_i2c_read_calls++;
    if (s_read_result != BMS_OK) { return s_read_result; }
    uint8_t idx = (reg_addr < MAX_REG) ? reg_addr : 0u;
    for (uint8_t i = 0u; i < len && i < 2u; i++) {
        buf[i] = s_reg_data[idx][i];
    }
    return BMS_OK;
}

BmsResult board_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr,
                               const uint8_t *data, uint8_t len) {
    (void)data; (void)len;
    s_last_dev_addr  = dev_addr;
    s_last_write_reg = reg_addr;
    mock_i2c_write_calls++;
    return s_write_result;
}
