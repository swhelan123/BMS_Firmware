/* test_config_validate.c — config validation unit tests. */
#include "unity.h"
#include "bms_config.h"
#include <string.h>

static BmsConfig make_valid_config(void) {
    BmsConfig cfg;
    bms_config_load_defaults(&cfg);
    return cfg;
}

void setUp(void) {}
void tearDown(void) {}

void test_validate_default_config_passes(void) {
    BmsConfig cfg = make_valid_config();
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_OK, bms_config_validate(&cfg, &err));
}

void test_validate_wrong_magic_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.magic = 0xDEADBEEFu;
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
    TEST_ASSERT_EQUAL(0u, err);
}

void test_validate_bad_crc_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.config_crc32 ^= 0xFFFFFFFFu; /* corrupt CRC */
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
}

void test_validate_wrong_hw_profile_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.hw_profile_id = 0x9999u;
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
    TEST_ASSERT_EQUAL(8u, err); /* hw_profile_id offset */
}

void test_validate_wrong_cell_count_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.cell_count = 70u;
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
}

void test_validate_inverted_uv_ov_fails(void) {
    BmsConfig cfg = make_valid_config();
    /* Invert UV/OV hard thresholds */
    uint16_t tmp = cfg.cell_uv_hard_mv;
    cfg.cell_uv_hard_mv = cfg.cell_ov_hard_mv;
    cfg.cell_ov_hard_mv = tmp;
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
}

void test_validate_mask_top_bits_set_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.required_cell_mask[9] |= 0x80u; /* set reserved bit */
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
    TEST_ASSERT_EQUAL(124u, err); /* required_cell_mask offset */
}

void test_validate_invalid_generation_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.config_generation = CONFIG_INVALID_GENERATION;
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
}

void test_validate_zero_overcurrent_hard_fails(void) {
    BmsConfig cfg = make_valid_config();
    cfg.overcurrent_hard_ma = 0u;
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
}

void test_config_size_is_226(void) {
    TEST_ASSERT_EQUAL(226u, sizeof(BmsConfig));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_validate_default_config_passes);
    RUN_TEST(test_validate_wrong_magic_fails);
    RUN_TEST(test_validate_bad_crc_fails);
    RUN_TEST(test_validate_wrong_hw_profile_fails);
    RUN_TEST(test_validate_wrong_cell_count_fails);
    RUN_TEST(test_validate_inverted_uv_ov_fails);
    RUN_TEST(test_validate_mask_top_bits_set_fails);
    RUN_TEST(test_validate_invalid_generation_fails);
    RUN_TEST(test_validate_zero_overcurrent_hard_fails);
    RUN_TEST(test_config_size_is_226);
    return UNITY_END();
}
