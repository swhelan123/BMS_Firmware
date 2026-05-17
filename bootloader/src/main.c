/* bootloader/src/main.c — STM32F303VC bootloader entry point.
 *
 * Boot decision (from docs/06_flash_and_bootloader.md):
 *   1. Check RTC BKP0R for boot flag → stay in bootloader if set.
 *   2. Validate application SP and reset vector.
 *   3. Verify application CRC against header stored at APP_START_ADDR.
 *   4. Jump to application.
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
        *bkp0r = 0u;
        return true;
    }
    return false;
}

static uint32_t bl_read_mcu_dev_id(void) {
    volatile uint32_t *idcode = (volatile uint32_t *)DBGMCU_IDCODE_ADDR;
    return *idcode & DBGMCU_DEV_ID_MASK;
}

int main(void) {
    bl_clock_init();

    if (bl_check_boot_flag()) {
        bl_uart_init();
        bl_protocol_run();
        /* bl_protocol_run() never returns */
    }

    volatile uint32_t *vtable = (volatile uint32_t *)APP_START_ADDR;
    uint32_t app_sp = vtable[0];
    uint32_t app_rv = vtable[1];

    if (!bl_is_valid_sp(app_sp) || !bl_is_valid_reset_vector(app_rv)) {
        bl_uart_init();
        bl_protocol_run();
    }

    /* Validate firmware package header CRC and fields */
    const FirmwarePackageHeader *hdr = (const FirmwarePackageHeader *)APP_START_ADDR;
    BlValidateResult vr = bl_validate_package_header(hdr, bl_read_mcu_dev_id());
    if (vr != BL_VALIDATE_OK) {
        bl_uart_init();
        bl_protocol_run();
    }

    /* Verify application image CRC */
    uint32_t computed = bl_flash_crc32_region(APP_START_ADDR, hdr->app_size);
    if (computed != hdr->app_crc32) {
        bl_uart_init();
        bl_protocol_run();
    }

    bl_jump_to_app(APP_START_ADDR);
}
