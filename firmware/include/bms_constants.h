/* bms_constants.h — single source of truth for all cross-layer constants.
 * All magic numbers, flash addresses, counts, protocol IDs, and timing
 * constants belong here. Other modules #include this header; never inline
 * a raw literal where a named constant exists.
 *
 * OPEN QUESTION items are flagged with OQ- prefix in comments.
 */
#pragma once

#include <stdint.h>

/* ── Hardware profile ─────────────────────────────────────────────────────── */
/* OQ-HW1: Confirm hw_profile_id value after hardware v1 sign-off. */
#define HW_PROFILE_ID               ((uint16_t)0x0001u)

/* ── MCU identity ─────────────────────────────────────────────────────────── */
/* STM32F303VC DBGMCU_IDCODE at 0xE0042000: DEV_ID=0x422, REV_ID varies.
 * Mask comparison: only lower 12 bits (DEV_ID) are stable across revisions. */
#define STM32F303VC_DEV_ID          (0x422u)
#define DBGMCU_IDCODE_ADDR          (0xE0042000u)

/* ── Flash map — STM32F303VC, 256 KB ──────────────────────────────────────── */
/* PROVISIONAL: verify page sizes on exact device before finalising linker. */
#define FLASH_BASE_ADDR             (0x08000000u)
#define FLASH_SIZE_BYTES            (256u * 1024u)
#define FLASH_PAGE_SIZE             (2048u)         /* 2 KB pages, F303xC */

#define BL_START_ADDR               (0x08000000u)
#define BL_SIZE_BYTES               (32u * 1024u)

#define APP_START_ADDR              (0x08008000u)
#define APP_REGION_SIZE             (188u * 1024u)  /* ends at 0x08037000 */

#define CONFIG_A_START_ADDR         (0x08037000u)
#define CONFIG_B_START_ADDR         (0x08039000u)
#define CONFIG_SLOT_SIZE            (8u * 1024u)

/* ── RAM map ──────────────────────────────────────────────────────────────── */
#define SRAM_BASE_ADDR              (0x20000000u)
#define SRAM_SIZE_BYTES             (40u * 1024u)

/* ── Topology — fixed for HW_PROFILE_ID 1 ────────────────────────────────── */
#define CELL_IC_COUNT               (5u)   /* number of LTC6812 ICs on CELL chain */
#define TEMP_IC_COUNT               (5u)   /* number of LTC6812 ICs on TEMP chain */
#define CELLS_PER_IC                (15u)
#define TEMPS_PER_IC                (15u)
#define TOTAL_CELL_COUNT            (CELL_IC_COUNT * CELLS_PER_IC)   /* 75 */
#define TOTAL_TEMP_COUNT            (TEMP_IC_COUNT * TEMPS_PER_IC)   /* 75 */

/* ── Config schema ────────────────────────────────────────────────────────── */
#define CONFIG_MAGIC                (0xBBCC0001u)
#define CONFIG_SCHEMA_VERSION       ((uint16_t)1u)
#define CONFIG_SCHEMA_SIZE          ((uint16_t)226u)
#define CONFIG_INVALID_GENERATION   (0xFFFFFFFFu)

/* 75-bit mask stored in 10 bytes; top 5 bits (75–79) must be zero */
#define CONFIG_MASK_BYTES           (10u)
#define CONFIG_MASK_RESERVED_SHIFT  (3u)   /* byte[9] bits [7:3] must be 0 */
#define CONFIG_MASK_RESERVED_MASK   (0xF8u)

/* ── Protocol ─────────────────────────────────────────────────────────────── */
#define PROTOCOL_VERSION            ((uint16_t)1u)
#define PROTOCOL_SOF_0              (0xAAu)
#define PROTOCOL_SOF_1              (0x55u)
#define PROTOCOL_FRAME_OVERHEAD     (10u)  /* SOF(2)+PKT_ID(2)+FLAGS(1)+SEQ(1)+LEN(2)+CRC(2) */
#define PROTOCOL_MAX_PAYLOAD        (512u) /* must be power of 2; log2 = 9 */
#define PROTOCOL_MAX_PAYLOAD_LOG2   (9u)
#define PROTOCOL_FLAGS_IS_RESPONSE  (0x01u)
#define PROTOCOL_FLAGS_IS_ERROR     (0x02u)

/* ── Firmware type codes ──────────────────────────────────────────────────── */
#define FIRMWARE_TYPE_BMS_APP       ((uint16_t)0x0001u)
#define FIRMWARE_TYPE_BOOTLOADER    ((uint16_t)0x0002u)

/* ── Feature flags ────────────────────────────────────────────────────────── */
#define FEAT_CELL_VOLTAGE           (1u << 0)
#define FEAT_TEMPERATURE            (1u << 1)
#define FEAT_BALANCING              (1u << 2)
#define FEAT_BOOTLOADER             (1u << 3)
#define FEAT_CAN                    (1u << 4)

/* Capabilities bitmask reported by BMS application */
#define BMS_APP_FEATURE_FLAGS       (FEAT_CELL_VOLTAGE | FEAT_TEMPERATURE | FEAT_BALANCING | FEAT_CAN)

/* ── Firmware version (update via cmake or version.h gen) ────────────────── */
#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR            (0u)
#define FW_VERSION_MINOR            (1u)
#define FW_VERSION_PATCH            (0u)
#endif

/* ── Firmware package format ──────────────────────────────────────────────── */
#define PKG_MAGIC                   (0xBF00BF00u)
#define PKG_HEADER_SIZE             (64u)
#define PKG_MAX_VERSION             ((uint16_t)1u)

/* ── Boot flag (RTC backup register) ─────────────────────────────────────── */
#define BL_ENTRY_FLAG               (0xB007B007u)

/* ── UART ─────────────────────────────────────────────────────────────────── */
#define UART_BAUD_RATE              (115200u)

/* ── Timing defaults (may be overridden by config) ───────────────────────── */
#define DEFAULT_STALE_DATA_TIMEOUT_MS   (500u)
#define DEFAULT_TEMP_SETTLE_MS          (5u)    /* OQ-TMP: validate with Enepaq datasheet */
#define MAIN_LOOP_PERIOD_MS             (10u)
#define IWDG_TIMEOUT_MS                 (500u)

/* ── State machine / inputs ──────────────────────────────────────────────── */
/* Charge-detect debounce: GPIO level must be stable this long before the
 * state machine sees the change. */
#define CHARGE_DETECT_DEBOUNCE_MS       (50u)

/* Periodic open-wire scan interval. Scans run only in STANDBY and CHARGE
 * states: ADOW conversions perturb cell readings, and a false trip while
 * the vehicle is being driven must be avoided. */
#define OPENWIRE_SCAN_PERIOD_MS         (10000u)

/* ── LTC6812 limits ───────────────────────────────────────────────────────── */
#define LTC6812_CELL_VOLTAGE_LSB_UV     (100u)   /* 100 µV per LSB */
#define LTC6812_MAX_RETRIES             (3u)
#define LTC6812_PEC_BYTES               (2u)
#define LTC6812_REG_GROUP_BYTES         (6u)     /* 6 data bytes per register group */

/* ── CMSIS static-assert helper ──────────────────────────────────────────── */
#define BMS_STATIC_ASSERT(expr, msg)   _Static_assert(expr, msg)

BMS_STATIC_ASSERT(TOTAL_CELL_COUNT == 75, "cell count must be 75");
BMS_STATIC_ASSERT(TOTAL_TEMP_COUNT == 75, "temp count must be 75");
BMS_STATIC_ASSERT(CONFIG_SCHEMA_SIZE == 226, "config size must match YAML");
BMS_STATIC_ASSERT(APP_START_ADDR + APP_REGION_SIZE <= CONFIG_A_START_ADDR,
                  "app region must not overlap config region");
BMS_STATIC_ASSERT(BL_START_ADDR + BL_SIZE_BYTES <= APP_START_ADDR,
                  "bootloader region must not overlap app region");
