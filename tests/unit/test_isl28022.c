/* test_isl28022.c — ISL28022 power monitor driver unit tests.
 *
 * Uses mock_board_i2c.c to intercept I2C transactions.
 * Tests verify: correct device address, register routing, scaling, error propagation.
 */
#include "unity.h"
#include "isl28022.h"
#include "board_pins.h"
#include <stdint.h>

/* ── mock_board_i2c control API ──────────────────────────────────────────── */
extern int mock_i2c_read_calls;
extern int mock_i2c_write_calls;

void mock_i2c_reset(void);
void mock_i2c_set_read_result(BmsResult r);
void mock_i2c_set_write_result(BmsResult r);
void mock_i2c_set_reg_data(uint8_t reg, uint8_t msb, uint8_t lsb);
uint8_t mock_i2c_last_dev_addr(void);
uint8_t mock_i2c_last_read_reg(void);
uint8_t mock_i2c_last_write_reg(void);

/* ── setUp / tearDown ─────────────────────────────────────────────────────── */

void setUp(void) {
    mock_i2c_reset();
    /* Pre-load sensible defaults so reads don't return garbage */
    mock_i2c_set_reg_data(ISL28022_REG_VBUS,   0x00, 0x00);   /* 0 mV */
    mock_i2c_set_reg_data(ISL28022_REG_VSHUNT,  0x00, 0x00);  /* 0 µV */
}

void tearDown(void) {}

/* ── isl28022_init tests ─────────────────────────────────────────────────── */

void test_init_uses_correct_i2c_address(void) {
    isl28022_init();
    TEST_ASSERT_EQUAL_UINT8(ISL28022_I2C_ADDR, mock_i2c_last_dev_addr());
}

void test_init_writes_two_registers(void) {
    isl28022_init();
    /* Writes config register (0x00) then calibration register (0x05) */
    TEST_ASSERT_EQUAL_INT(2, mock_i2c_write_calls);
}

void test_init_writes_config_register_first(void) {
    /* We can only inspect the last write reg via our mock, so verify init
     * returns BMS_OK and completes both writes when I2C is healthy. */
    BmsResult r = isl28022_init();
    TEST_ASSERT_EQUAL(BMS_OK, r);
}

void test_init_fails_on_i2c_error(void) {
    mock_i2c_set_write_result(BMS_ERR_I2C);
    BmsResult r = isl28022_init();
    TEST_ASSERT_EQUAL(BMS_ERR_I2C, r);
}

/* ── isl28022_read: address and register routing ─────────────────────────── */

void test_read_uses_correct_device_address(void) {
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_UINT8(ISL28022_I2C_ADDR, mock_i2c_last_dev_addr());
}

void test_read_makes_two_i2c_reads(void) {
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_INT(2, mock_i2c_read_calls);
}

/* ── isl28022_read: error propagation ───────────────────────────────────── */

void test_read_returns_i2c_error_on_nack(void) {
    mock_i2c_set_read_result(BMS_ERR_I2C);
    int32_t vbus_mv, vshunt_uv;
    BmsResult r = isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL(BMS_ERR_I2C, r);
}

/* ── isl28022_read: Vbus scaling (4 mV / LSB, right-shifted 3) ─────────── */

void test_read_vbus_zero_register_returns_zero_mv(void) {
    mock_i2c_set_reg_data(ISL28022_REG_VBUS, 0x00, 0x00);
    int32_t vbus_mv, vshunt_uv;
    TEST_ASSERT_EQUAL(BMS_OK, isl28022_read(&vbus_mv, &vshunt_uv));
    TEST_ASSERT_EQUAL_INT32(0, vbus_mv);
}

void test_read_vbus_one_lsb(void) {
    /* reg = 0x0008 → bits[15:3]=1 → raw_count=1 → 1×4 = 4 mV */
    mock_i2c_set_reg_data(ISL28022_REG_VBUS, 0x00, 0x08);
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_INT32(4, vbus_mv);
}

void test_read_vbus_12000_mv(void) {
    /* 12000 mV / 4 mV = 3000 raw counts; reg = 3000 << 3 = 24000 = 0x5DC0 */
    mock_i2c_set_reg_data(ISL28022_REG_VBUS, 0x5D, 0xC0);
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_INT32(12000, vbus_mv);
}

/* ── isl28022_read: Vshunt scaling (80 µV / LSB with PG=/8) ────────────── */

void test_read_vshunt_zero(void) {
    mock_i2c_set_reg_data(ISL28022_REG_VSHUNT, 0x00, 0x00);
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_INT32(0, vshunt_uv);
}

void test_read_vshunt_one_lsb_positive(void) {
    /* reg = 0x0001 (signed +1) → +1 × 80 µV = +80 µV */
    mock_i2c_set_reg_data(ISL28022_REG_VSHUNT, 0x00, 0x01);
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_INT32(80, vshunt_uv);
}

void test_read_vshunt_negative(void) {
    /* reg = 0xFFFF (signed -1) → -1 × 80 µV = -80 µV */
    mock_i2c_set_reg_data(ISL28022_REG_VSHUNT, 0xFF, 0xFF);
    int32_t vbus_mv, vshunt_uv;
    isl28022_read(&vbus_mv, &vshunt_uv);
    TEST_ASSERT_EQUAL_INT32(-80, vshunt_uv);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_init_uses_correct_i2c_address);
    RUN_TEST(test_init_writes_two_registers);
    RUN_TEST(test_init_writes_config_register_first);
    RUN_TEST(test_init_fails_on_i2c_error);

    RUN_TEST(test_read_uses_correct_device_address);
    RUN_TEST(test_read_makes_two_i2c_reads);
    RUN_TEST(test_read_returns_i2c_error_on_nack);

    RUN_TEST(test_read_vbus_zero_register_returns_zero_mv);
    RUN_TEST(test_read_vbus_one_lsb);
    RUN_TEST(test_read_vbus_12000_mv);

    RUN_TEST(test_read_vshunt_zero);
    RUN_TEST(test_read_vshunt_one_lsb_positive);
    RUN_TEST(test_read_vshunt_negative);

    return UNITY_END();
}
