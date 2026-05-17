/* board_i2c.h — I2C2 driver for ISL28022 (PA9=SCL, PA10=SDA, AF4, 100 kHz). */
#pragma once
#include <stdint.h>
#include "bms_types.h"

/* Init I2C2 peripheral. */
void board_i2c_init(void);

/* Blocking register read: write reg_addr byte, then read len bytes into buf.
 * Returns BMS_OK or BMS_ERR_I2C on NACK or timeout. */
BmsResult board_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t *buf, uint8_t len);

/* Blocking register write: write reg_addr + data bytes.
 * Returns BMS_OK or BMS_ERR_I2C. */
BmsResult board_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr,
                               const uint8_t *data, uint8_t len);
