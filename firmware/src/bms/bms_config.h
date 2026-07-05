/* bms_config.h — config struct and config-management API.
 *
 * Config is loaded once at boot from flash. All other modules receive a
 * const pointer via bms_config_get(). The pointer is valid for the lifetime
 * of the application after a successful bms_config_load().
 *
 * Struct layout must match protocol/config_schema.yaml exactly.
 * If you add a field, update the YAML first, then this struct.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bms_types.h"
#include "bms_constants.h"

/* ── Config struct (226 bytes, packed, little-endian) ─────────────────────── */
#pragma pack(push, 1)
typedef struct {
    /* Header (64 bytes, offset 0) */
    uint32_t magic;                  /* 0xBBCC0001 */
    uint16_t schema_version;         /* must == CONFIG_SCHEMA_VERSION */
    uint16_t total_length;           /* must == CONFIG_SCHEMA_SIZE */
    uint16_t hw_profile_id;          /* must == HW_PROFILE_ID */
    uint32_t config_generation;      /* wraps; dual-slot selector */
    uint32_t config_crc32;           /* CRC-32/ISO-HDLC; this field = 0 during calculation */
    uint8_t  reserved_header[46];

    /* Topology (4 bytes, offset 64) */
    uint8_t  cell_count;             /* must == 75 */
    uint8_t  temp_count;             /* must == 75 */
    uint16_t reserved_topology;

    /* Cell voltage thresholds (16 bytes, offset 68) */
    uint16_t cell_uv_hard_mv;
    uint16_t cell_uv_soft_mv;
    uint16_t cell_ov_soft_mv;
    uint16_t cell_ov_hard_mv;
    uint16_t cell_balance_target_mv;
    uint16_t cell_balance_hysteresis_mv;
    uint16_t cell_nominal_mv;
    uint16_t reserved_cell_thresholds;

    /* Temperature thresholds (16 bytes, offset 84) */
    int16_t  temp_charge_warn_cx10;
    int16_t  temp_charge_hard_cx10;
    int16_t  temp_discharge_warn_cx10;
    int16_t  temp_discharge_hard_cx10;
    int16_t  temp_hard_abs_cx10;
    int16_t  temp_cold_charge_limit_cx10;
    int16_t  temp_cold_discharge_limit_cx10;
    uint16_t reserved_temp_thresholds;

    /* Current limits (8 bytes, offset 100) */
    uint32_t overcurrent_hard_ma;
    uint32_t overcurrent_warn_ma;

    /* Balancing (8 bytes, offset 108) */
    uint32_t balance_on_time_ms;
    uint32_t balance_off_time_ms;

    /* Temperature measurement (4 bytes, offset 116) */
    uint16_t temp_settle_time_ms;
    uint16_t reserved_temp_params;

    /* Stale data timeout (4 bytes, offset 120) */
    uint32_t stale_data_timeout_ms;

    /* Masks (30 bytes, offset 124): 75-bit masks in 10 bytes each */
    uint8_t required_cell_mask[10];
    uint8_t required_temp_mask[10];
    uint8_t balance_allowed_mask[10];

    /* Calibration (18 bytes, offset 154) */
    uint32_t vpack_gain_x1000;
    int32_t  vpack_offset_mv;
    uint16_t vbat_gain_x1000;
    int16_t  vbat_offset_mv;
    uint32_t current_gain_x1000;   /* uint32: AMC1302+divider chain requires ~1,855,000 */
    int16_t  current_offset_ma;

    /* CAN / communication (8 bytes, offset 172) */
    uint32_t can_watchdog_timeout_ms;
    uint16_t can_base_id;
    uint16_t reserved_can;

    /* Capacity (4 bytes, offset 180) */
    uint32_t capacity_mah;           /* pack capacity in mAh; used for SOC coulomb counting */

    /* Reserved (42 bytes, offset 184) */
    uint8_t  reserved[42];
} BmsConfig;
#pragma pack(pop)

_Static_assert(sizeof(BmsConfig) == CONFIG_SCHEMA_SIZE,
               "BmsConfig size must match CONFIG_SCHEMA_SIZE");

/* ── Config API ──────────────────────────────────────────────────────────── */

/* Load config from flash dual-slot. Selects highest valid generation.
 * Falls back to compiled-in safe defaults on error.
 * Must be called once at boot before bms_config_get(). */
BmsResult bms_config_load(void);

/* Returns pointer to the active config. Valid after bms_config_load(). */
const BmsConfig *bms_config_get(void);

/* Validate a candidate config blob. Returns BMS_OK or BMS_ERR_CONFIG_INVALID.
 * Sets err_field_offset to the byte offset of the first failing field, or 0xFFFF. */
BmsResult bms_config_validate(const BmsConfig *cfg, uint16_t *err_field_offset);

/* Apply a validated config to RAM (no flash write). Reverts on reset. */
BmsResult bms_config_apply_ram(const BmsConfig *cfg);

/* Validate and write config to flash (dual-slot). Performs soft reset after write. */
BmsResult bms_config_store(const BmsConfig *cfg);

/* Write compile-time safe defaults to the active config RAM slot. */
void bms_config_load_defaults(BmsConfig *out);

/* CRC-32/ISO-HDLC over bytes [0..CONFIG_SCHEMA_SIZE-5], config_crc32 field zeroed. */
uint32_t bms_config_compute_crc(const BmsConfig *cfg);

/* Number of LTC6812 ICs (segments) actually populated, from the active config.
 * Derived from cell_count / CELLS_PER_IC; always in [MIN_CELL_IC_COUNT,
 * CELL_IC_COUNT]. Use these — not CELL_IC_COUNT — to bound isoSPI chain reads
 * and measurement population so the same image serves 4- and 5-segment packs. */
uint8_t bms_config_active_cell_ics(void);
uint8_t bms_config_active_temp_ics(void);
