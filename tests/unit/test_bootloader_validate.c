/* test_bootloader_validate.c — firmware package header validation tests. */
#include "unity.h"
#include "bl_validate.h"
#include "bl_config.h"
#include <string.h>
#include <stdint.h>

#define TEST_MCU_DEV_ID  (STM32F303VC_DEV_ID)

static FirmwarePackageHeader make_valid_header(void) {
    FirmwarePackageHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkg_magic            = PKG_MAGIC;
    hdr.pkg_version          = 1u;
    hdr.hw_profile_id        = HW_PROFILE_ID;
    hdr.target_mcu_id        = TEST_MCU_DEV_ID;
    hdr.image_type           = 0x01u;
    hdr.app_start_addr       = APP_START_ADDR;
    hdr.app_size             = 4096u;  /* small valid size */
    hdr.app_crc32            = 0xDEADu; /* placeholder; not validated here */
    hdr.fw_version[0]        = 1u;
    hdr.min_bootloader_version[0] = 0u;
    hdr.required_config_schema    = 1u;
    /* Compute header CRC */
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    return hdr;
}

void setUp(void) {}
void tearDown(void) {}

void test_valid_header_passes(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    TEST_ASSERT_EQUAL(BL_VALIDATE_OK, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_bad_magic_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.pkg_magic = 0xDEADBEEFu;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_BAD_MAGIC, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_wrong_hw_profile_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.hw_profile_id = 0xFFFFu;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_HW_PROFILE, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_wrong_mcu_id_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.target_mcu_id = 0x999u; /* different DEV_ID */
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_MCU_ID, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_wrong_image_type_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.image_type = 0x02u; /* bootloader type, not application */
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_IMAGE_TYPE, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_wrong_app_addr_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.app_start_addr = 0x08000000u; /* bootloader address, not app */
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_APP_ADDR, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_zero_app_size_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.app_size = 0u;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_APP_SIZE, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_oversized_app_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.app_size = APP_REGION_SIZE + 1u;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_APP_SIZE, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_app_size_above_max_fails(void) {
    /* One byte into the metadata page must be rejected */
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.app_size = APP_MAX_SIZE + 1u;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_ERR_APP_SIZE, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_app_size_at_max_passes(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.app_size = APP_MAX_SIZE;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    TEST_ASSERT_EQUAL(BL_VALIDATE_OK, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_corrupted_header_crc_fails(void) {
    FirmwarePackageHeader hdr = make_valid_header();
    hdr.pkg_header_crc32 ^= 0x1u;
    TEST_ASSERT_EQUAL(BL_ERR_HEADER_CRC, bl_validate_package_header(&hdr, TEST_MCU_DEV_ID));
}

void test_header_size_is_64(void) {
    TEST_ASSERT_EQUAL(64u, sizeof(FirmwarePackageHeader));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_header_passes);
    RUN_TEST(test_bad_magic_fails);
    RUN_TEST(test_wrong_hw_profile_fails);
    RUN_TEST(test_wrong_mcu_id_fails);
    RUN_TEST(test_wrong_image_type_fails);
    RUN_TEST(test_wrong_app_addr_fails);
    RUN_TEST(test_zero_app_size_fails);
    RUN_TEST(test_oversized_app_fails);
    RUN_TEST(test_app_size_above_max_fails);
    RUN_TEST(test_app_size_at_max_passes);
    RUN_TEST(test_corrupted_header_crc_fails);
    RUN_TEST(test_header_size_is_64);
    return UNITY_END();
}
