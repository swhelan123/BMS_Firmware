/* bootloader/src/main.c — STM32F303VC bootloader entry point.
 *
 * Boot decision (see docs/06_flash_and_bootloader.md):
 *   1. RTC BKP0R boot flag set              → stay in bootloader.
 *   2. App SP / reset vector invalid        → stay in bootloader.
 *   3. Metadata word == BL_META_UPDATING    → interrupted update; stay in bootloader.
 *   4. Metadata word == PKG_MAGIC           → validate persisted header + app CRC;
 *                                             any failure → stay in bootloader.
 *   5. Otherwise (no metadata, e.g. SWD-flashed app) or all checks pass → jump.
 *
 * The raw application image lives at APP_START_ADDR (vector table first).
 * The 64-byte package header is persisted separately at APP_META_ADDR by
 * BOOT_UPDATE_FINALIZE. An SWD-flashed app has no metadata and boots on
 * valid vectors alone.
 */
#include "bl_config.h"
#include "bl_validate.h"
#include "bl_jump.h"
#include "bl_uart.h"
#include "bl_flash.h"
#include "bl_protocol.h"
#include <stdint.h>
#include <stdbool.h>

static void bl_clock_init(void) {
    /* HSI already on at reset — no action needed for 115200 baud at 8 MHz */
}

static bool bl_check_boot_flag(void) {
    volatile uint32_t *bkp0r = (volatile uint32_t *)0x40002850u; /* RTC BKP0R */
    if (*bkp0r == BL_ENTRY_FLAG) {
        /* Clearing the flag is a backup-domain write: unlock it first
         * (PWR clock + DBP), or the write is silently ignored and the
         * board would re-enter the bootloader on every reset. */
        volatile uint32_t *rcc_apb1enr = (volatile uint32_t *)0x4002101Cu;
        volatile uint32_t *pwr_cr      = (volatile uint32_t *)0x40007000u;
        *rcc_apb1enr |= (1u << 28);   /* PWREN */
        *pwr_cr      |= (1u << 8);    /* DBP   */
        *bkp0r = 0u;
        return true;
    }
    return false;
}

static uint32_t bl_read_mcu_dev_id(void) {
    volatile uint32_t *idcode = (volatile uint32_t *)DBGMCU_IDCODE_ADDR;
    return *idcode & DBGMCU_DEV_ID_MASK;
}

static void bl_stay_resident(void) {
    bl_uart_init();
    bl_protocol_run();
    /* bl_protocol_run() never returns */
}

int main(void) {
    bl_clock_init();

    /* 1. Explicit bootloader entry requested by the application */
    if (bl_check_boot_flag()) {
        bl_stay_resident();
    }

    /* 2. Application vector table must be sane in every path */
    volatile uint32_t *vtable = (volatile uint32_t *)APP_START_ADDR;
    uint32_t app_sp = vtable[0];
    uint32_t app_rv = vtable[1];
    if (!bl_is_valid_sp(app_sp) || !bl_is_valid_reset_vector(app_rv)) {
        bl_stay_resident();
    }

    /* 3./4. Metadata page decides how much further validation is possible */
    uint32_t meta_word = *(volatile uint32_t *)APP_META_ADDR;

    if (meta_word == BL_META_UPDATING) {
        /* BEGIN ran but FINALIZE never persisted a header — image is suspect */
        bl_stay_resident();
    }

    if (meta_word == PKG_MAGIC) {
        const FirmwarePackageHeader *hdr =
            (const FirmwarePackageHeader *)APP_META_ADDR;

        if (bl_validate_package_header(hdr, bl_read_mcu_dev_id()) != BL_VALIDATE_OK) {
            bl_stay_resident();
        }
        if (bl_flash_crc32_region(APP_START_ADDR, hdr->app_size) != hdr->app_crc32) {
            bl_stay_resident();
        }
    }
    /* else: no metadata (erased page / SWD-flashed app) — vectors already
     * validated in step 2; boot without CRC coverage. */

    /* 5. Hand over */
    bl_jump_to_app(APP_START_ADDR);
}
