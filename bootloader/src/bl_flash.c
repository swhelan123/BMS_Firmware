/* bl_flash.c — STM32F303 flash erase/write for bootloader.
 *
 * All operations are guarded by bl_flash_addr_in_app_region().
 * In BMS_HOST_BUILD the hardware operations are replaced by a byte-array simulation.
 */
#include "bl_flash.h"
#include "bl_validate.h"   /* bl_crc32 */
#include "bl_config.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Compile-time region safety check */
_Static_assert(APP_START_ADDR + APP_REGION_SIZE <= CONFIG_A_START_ADDR,
               "app region must not overlap config region");

bool bl_flash_addr_in_app_region(uint32_t addr, uint32_t len) {
    return (addr >= APP_START_ADDR)
        && (len  <= APP_REGION_SIZE)
        && (addr + len <= APP_START_ADDR + APP_REGION_SIZE);
}

/* ══════════════════════════════════════════════════════════════════════════ */
#ifndef BMS_HOST_BUILD
/* ══════════════════════════════════════════════════════════════════════════ */

/* STM32F303 Flash registers (RM0316 §4) */
#define FLASH_KEYR  (*(volatile uint32_t *)0x40022004u)
#define FLASH_SR    (*(volatile uint32_t *)0x4002200Cu)
#define FLASH_CR    (*(volatile uint32_t *)0x40022010u)
#define FLASH_AR    (*(volatile uint32_t *)0x40022014u)

#define FLASH_KEY1   0x45670123u
#define FLASH_KEY2   0xCDEF89ABu

#define FLASH_CR_PG    (1u << 0)
#define FLASH_CR_PER   (1u << 1)
#define FLASH_CR_STRT  (1u << 6)
#define FLASH_CR_LOCK  (1u << 7)

#define FLASH_SR_BSY   (1u << 0)
#define FLASH_SR_PGERR (1u << 2)
#define FLASH_SR_WRPRT (1u << 4)
#define FLASH_SR_EOP   (1u << 5)

static void flash_unlock(void) {
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static void flash_lock(void) { FLASH_CR |= FLASH_CR_LOCK; }

static BlFlashResult flash_wait(void) {
    while (FLASH_SR & FLASH_SR_BSY) {}
    if (FLASH_SR & FLASH_SR_PGERR) { return BL_FLASH_ERR_PGERR; }
    if (FLASH_SR & FLASH_SR_WRPRT) { return BL_FLASH_ERR_WRPRT; }
    return BL_FLASH_OK;
}

BlFlashResult bl_flash_erase_page(uint32_t addr) {
    if ((addr % FLASH_PAGE_SIZE) != 0u) { return BL_FLASH_ERR_ALIGN; }
    if (!bl_flash_addr_in_app_region(addr, FLASH_PAGE_SIZE)) { return BL_FLASH_ERR_RANGE; }

    flash_unlock();
    BlFlashResult r = flash_wait();
    if (r != BL_FLASH_OK) { flash_lock(); return r; }

    FLASH_SR |= FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRT;
    FLASH_CR |= FLASH_CR_PER;
    FLASH_AR  = addr;
    FLASH_CR |= FLASH_CR_STRT;

    r = flash_wait();
    FLASH_CR &= ~FLASH_CR_PER;
    flash_lock();
    if (r != BL_FLASH_OK) { return r; }

    /* Spot-check: first word must read 0xFFFFFFFF */
    if (*(volatile uint32_t *)addr != 0xFFFFFFFFu) { return BL_FLASH_ERR_VERIFY; }
    return BL_FLASH_OK;
}

BlFlashResult bl_flash_write_halfword(uint32_t addr, uint16_t val) {
    if ((addr & 1u) != 0u) { return BL_FLASH_ERR_ALIGN; }
    if (!bl_flash_addr_in_app_region(addr, 2u)) { return BL_FLASH_ERR_RANGE; }

    flash_unlock();
    BlFlashResult r = flash_wait();
    if (r != BL_FLASH_OK) { flash_lock(); return r; }

    FLASH_SR |= FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRT;
    FLASH_CR |= FLASH_CR_PG;
    *(volatile uint16_t *)addr = val;

    r = flash_wait();
    FLASH_CR &= ~FLASH_CR_PG;
    flash_lock();
    if (r != BL_FLASH_OK) { return r; }

    if (*(volatile uint16_t *)addr != val) { return BL_FLASH_ERR_VERIFY; }
    return BL_FLASH_OK;
}

uint32_t bl_flash_crc32_region(uint32_t start_addr, uint32_t len) {
    return bl_crc32((const uint8_t *)start_addr, len);
}

/* ══════════════════════════════════════════════════════════════════════════ */
#else  /* BMS_HOST_BUILD — byte-array flash simulation */
/* ══════════════════════════════════════════════════════════════════════════ */

#define FLASH_SIM_SIZE  (APP_REGION_SIZE + 4096u)

static uint8_t  s_sim[FLASH_SIM_SIZE];
static bool     s_initialized = false;

static void sim_init(void) {
    if (!s_initialized) {
        memset(s_sim, 0xFF, sizeof(s_sim));
        s_initialized = true;
    }
}

void bl_flash_sim_reset(void) {
    memset(s_sim, 0xFF, sizeof(s_sim));
    s_initialized = true;
}

const uint8_t *bl_flash_sim_ptr(void) {
    sim_init();
    return s_sim;
}

BlFlashResult bl_flash_erase_page(uint32_t addr) {
    sim_init();
    if ((addr % FLASH_PAGE_SIZE) != 0u) { return BL_FLASH_ERR_ALIGN; }
    if (!bl_flash_addr_in_app_region(addr, FLASH_PAGE_SIZE)) { return BL_FLASH_ERR_RANGE; }
    memset(&s_sim[addr - APP_START_ADDR], 0xFF, FLASH_PAGE_SIZE);
    return BL_FLASH_OK;
}

BlFlashResult bl_flash_write_halfword(uint32_t addr, uint16_t val) {
    sim_init();
    if ((addr & 1u) != 0u) { return BL_FLASH_ERR_ALIGN; }
    if (!bl_flash_addr_in_app_region(addr, 2u)) { return BL_FLASH_ERR_RANGE; }
    uint32_t off = addr - APP_START_ADDR;
    s_sim[off]   = (uint8_t)(val & 0xFFu);
    s_sim[off+1] = (uint8_t)((val >> 8) & 0xFFu);
    return BL_FLASH_OK;
}

uint32_t bl_flash_crc32_region(uint32_t start_addr, uint32_t len) {
    sim_init();
    if (start_addr < APP_START_ADDR) { return 0u; }
    uint32_t off = start_addr - APP_START_ADDR;
    if (off + len > FLASH_SIM_SIZE) { return 0u; }
    return bl_crc32(&s_sim[off], len);
}

#endif /* BMS_HOST_BUILD */
