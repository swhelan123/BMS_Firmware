/* bl_config.h — bootloader compile-time constants.
 * Shared with firmware application via bms_constants.h.
 * This header is also included by the host-compiled package validator tests.
 */
#pragma once
#include <stdint.h>

/* ── Bootloader version ─────────────────────────────────────────────────── */
#define BL_VERSION_MAJOR  (0u)
#define BL_VERSION_MINOR  (1u)
#define BL_VERSION_PATCH  (0u)

/* ── Flash map (shared with firmware/include/bms_constants.h) ──────────── */
#define BL_FLASH_BASE        (0x08000000u)
#define BL_START_ADDR        (0x08000000u)
#define BL_SIZE_BYTES        (32u * 1024u)
#define APP_START_ADDR       (0x08008000u)
#define APP_REGION_SIZE      (188u * 1024u)
#define CONFIG_A_START_ADDR  (0x08037000u)
#define CONFIG_B_START_ADDR  (0x08039000u)
#define CONFIG_SLOT_SIZE     (8u * 1024u)
#define FLASH_PAGE_SIZE      (2048u)

/* ── Package format ─────────────────────────────────────────────────────── */
#define PKG_MAGIC            (0xBF00BF00u)
#define PKG_HEADER_SIZE      (64u)
#define BL_MAX_PKG_VERSION   (1u)

/* ── Hardware profile ────────────────────────────────────────────────────── */
/* Must match HW_PROFILE_ID in bms_constants.h */
#ifndef HW_PROFILE_ID
#define HW_PROFILE_ID        (0x0001u)
#endif

/* ── MCU identity ────────────────────────────────────────────────────────── */
#define STM32F303VC_DEV_ID   (0x422u)
#define DBGMCU_IDCODE_ADDR   (0xE0042000u)
#define DBGMCU_DEV_ID_MASK   (0x00000FFFu)

/* ── Boot flag ───────────────────────────────────────────────────────────── */
#define BL_ENTRY_FLAG        (0xB007B007u)

/* ── Protocol version supported by this bootloader ───────────────────────── */
#define BL_PROTOCOL_VERSION  (1u)

/* ── Config schema version this bootloader knows about ───────────────────── */
#define BL_CONFIG_SCHEMA_VERSION (1u)

/* ── Compile-time safety assertions ─────────────────────────────────────── */
#ifndef __ASSEMBLER__
#include <assert.h>
_Static_assert(BL_START_ADDR + BL_SIZE_BYTES <= APP_START_ADDR,
               "bootloader must not overlap application region");
_Static_assert(APP_START_ADDR + APP_REGION_SIZE <= CONFIG_A_START_ADDR,
               "application must not overlap config region A");
_Static_assert(CONFIG_A_START_ADDR + CONFIG_SLOT_SIZE <= CONFIG_B_START_ADDR,
               "config slot A must not overlap slot B");
#endif
