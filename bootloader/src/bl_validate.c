/* bl_validate.c — firmware package header validation. */
#include "bl_validate.h"
#include "bl_config.h"
#include <string.h>

uint32_t bl_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

BlValidateResult bl_validate_package_header(const FirmwarePackageHeader *hdr,
                                             uint32_t mcu_dev_id) {
    /* Step 1: magic */
    if (hdr->pkg_magic != PKG_MAGIC) { return BL_ERR_BAD_MAGIC; }

    /* Step 2: package version */
    if (hdr->pkg_version > BL_MAX_PKG_VERSION || hdr->pkg_version == 0u) {
        return BL_ERR_PKG_VERSION;
    }

    /* Step 3: hardware profile */
    if (hdr->hw_profile_id != HW_PROFILE_ID) { return BL_ERR_HW_PROFILE; }

    /* Step 4: MCU identity — compare lower 12 bits (DEV_ID) */
    if ((hdr->target_mcu_id & DBGMCU_DEV_ID_MASK) != (mcu_dev_id & DBGMCU_DEV_ID_MASK)) {
        return BL_ERR_MCU_ID;
    }

    /* Step 5: image type */
    if (hdr->image_type != 0x01u) { return BL_ERR_IMAGE_TYPE; }

    /* Step 6: application start address */
    if (hdr->app_start_addr != APP_START_ADDR) { return BL_ERR_APP_ADDR; }

    /* Step 7: application size (must leave the metadata page untouched) */
    if (hdr->app_size == 0u || hdr->app_size > APP_MAX_SIZE) {
        return BL_ERR_APP_SIZE;
    }

    /* Step 8: header CRC32 — covers bytes [0x00..0x25] (header without crc field) */
    uint32_t computed = bl_crc32((const uint8_t *)hdr, 0x26u);
    if (computed != hdr->pkg_header_crc32) { return BL_ERR_HEADER_CRC; }

    /* Step 9: bootloader version requirement */
    uint32_t our_version = ((uint32_t)BL_VERSION_MAJOR << 16) |
                           ((uint32_t)BL_VERSION_MINOR << 8)  |
                            (uint32_t)BL_VERSION_PATCH;
    uint32_t req_version = ((uint32_t)hdr->min_bootloader_version[0] << 16) |
                           ((uint32_t)hdr->min_bootloader_version[1] << 8)  |
                            (uint32_t)hdr->min_bootloader_version[2];
    if (our_version < req_version) { return BL_ERR_BL_VERSION; }

    return BL_VALIDATE_OK;
}
