/* bl_flash.h — STM32F303 flash driver for bootloader (HAL-free, direct registers).
 *
 * Safety contract:
 *  - Only erases/writes within [APP_START_ADDR, APP_START_ADDR + APP_REGION_SIZE).
 *  - Never touches bootloader region (< APP_START_ADDR).
 *  - Never touches config region (>= CONFIG_A_START_ADDR).
 *  - Both constraints enforced by bl_flash_addr_in_app_region() on every call.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bl_config.h"

typedef enum {
    BL_FLASH_OK          = 0,
    BL_FLASH_ERR_RANGE   = 1,  /* address outside app region */
    BL_FLASH_ERR_ALIGN   = 2,  /* address not aligned */
    BL_FLASH_ERR_PGERR   = 3,  /* programming error (hardware) */
    BL_FLASH_ERR_WRPRT   = 4,  /* write-protect error */
    BL_FLASH_ERR_VERIFY  = 5,  /* readback mismatch */
} BlFlashResult;

/* True if [addr, addr+len) is entirely within the writable app region. */
bool bl_flash_addr_in_app_region(uint32_t addr, uint32_t len);

/* Erase one 2 KB page at addr (must be 2048-byte aligned, within app region). */
BlFlashResult bl_flash_erase_page(uint32_t addr);

/* Write one 16-bit half-word to addr (must be 2-byte aligned, within app region). */
BlFlashResult bl_flash_write_halfword(uint32_t addr, uint16_t val);

/* CRC-32/ISO-HDLC over flash bytes [start_addr, start_addr+len).
 * In host build reads from the simulation buffer. */
uint32_t bl_flash_crc32_region(uint32_t start_addr, uint32_t len);

/* ── Host-build simulation helpers (BMS_HOST_BUILD only) ─────────────────── */
#ifdef BMS_HOST_BUILD
void           bl_flash_sim_reset(void);
const uint8_t *bl_flash_sim_ptr(void);
#endif
