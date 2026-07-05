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

/* ── Flexible segment count (4- vs 5-segment packs on one image) ─────────── */

/* Set a whole-segment topology and trim every mask so bits [count..79] are
 * clear, then refresh the CRC — the shape a valid N-segment config must have. */
static void set_segments(BmsConfig *cfg, uint8_t segments) {
    uint8_t count = (uint8_t)(segments * 15u);
    cfg->cell_count = count;
    cfg->temp_count = count;
    for (uint8_t byte = 0; byte < 10u; byte++) {
        uint8_t keep = 0u;
        for (uint8_t bit = 0; bit < 8u; bit++) {
            uint16_t idx = (uint16_t)(byte * 8u + bit);
            if (idx < count) { keep |= (uint8_t)(1u << bit); }
        }
        cfg->required_cell_mask[byte]   = keep;
        cfg->required_temp_mask[byte]   = keep;
        cfg->balance_allowed_mask[byte] = keep;
    }
    cfg->config_crc32 = 0; cfg->config_crc32 = bms_config_compute_crc(cfg);
}

void test_validate_60cell_config_passes(void) {
    BmsConfig cfg = make_valid_config();
    set_segments(&cfg, 4u);           /* 60 cells / 60 temps */
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_OK, bms_config_validate(&cfg, &err));
}

void test_validate_non_segment_count_fails(void) {
    BmsConfig cfg = make_valid_config();
    set_segments(&cfg, 4u);
    cfg.cell_count = 61u;             /* not a multiple of 15 */
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
    TEST_ASSERT_EQUAL(64u, err);
}

void test_validate_cell_temp_mismatch_fails(void) {
    BmsConfig cfg = make_valid_config();
    set_segments(&cfg, 4u);
    cfg.temp_count = 75u;            /* != cell_count */
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
    TEST_ASSERT_EQUAL(65u, err);
}

void test_validate_required_mask_beyond_count_fails(void) {
    BmsConfig cfg = make_valid_config();
    set_segments(&cfg, 4u);
    cfg.required_cell_mask[7] |= (uint8_t)(1u << 4); /* bit 60 — absent cell */
    cfg.config_crc32 = 0; cfg.config_crc32 = bms_config_compute_crc(&cfg);
    uint16_t err;
    TEST_ASSERT_EQUAL(BMS_ERR_CONFIG_INVALID, bms_config_validate(&cfg, &err));
    TEST_ASSERT_EQUAL(124u, err);
}

void test_active_ics_tracks_config(void) {
    BmsConfig cfg = make_valid_config();
    set_segments(&cfg, 4u);
    TEST_ASSERT_EQUAL(BMS_OK, bms_config_apply_ram(&cfg));
    TEST_ASSERT_EQUAL(4u, bms_config_active_cell_ics());
    TEST_ASSERT_EQUAL(4u, bms_config_active_temp_ics());

    set_segments(&cfg, 5u);
    TEST_ASSERT_EQUAL(BMS_OK, bms_config_apply_ram(&cfg));
    TEST_ASSERT_EQUAL(5u, bms_config_active_cell_ics());
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
    RUN_TEST(test_validate_60cell_config_passes);
    RUN_TEST(test_validate_non_segment_count_fails);
    RUN_TEST(test_validate_cell_temp_mismatch_fails);
    RUN_TEST(test_validate_required_mask_beyond_count_fails);
    RUN_TEST(test_active_ics_tracks_config);
    return UNITY_END();
}
