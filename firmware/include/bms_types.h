/* bms_types.h — shared enums, structs, and measurement types used across
 * BMS application modules. Does not include hardware register definitions.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bms_constants.h"

/* ── BMS state machine ────────────────────────────────────────────────────── */
typedef enum {
    BMS_STATE_INIT          = 0,   /* startup, hardware not yet validated */
    BMS_STATE_STANDBY       = 1,   /* idle; MasterOk (health) asserted, no charge/discharge */
    BMS_STATE_DISCHARGE     = 2,   /* discharge path active */
    BMS_STATE_CHARGE        = 3,   /* charge path active */
    BMS_STATE_FAULT         = 4,   /* fatal or blocking fault present */
    BMS_STATE_SHUTDOWN      = 5,   /* power-down sequence */
} BmsState;

/* ── Chain identifiers ────────────────────────────────────────────────────── */
typedef enum {
    BMS_CHAIN_CELL = 0,   /* LTC6812 cell measurement chain, CS = PA4  */
    BMS_CHAIN_TEMP = 1,   /* LTC6812 temperature chain,     CS = PB12 */
} BmsChain;

/* ── Result codes (module-level) ──────────────────────────────────────────── */
typedef enum {
    BMS_OK                  = 0,
    BMS_ERR_TIMEOUT         = 1,
    BMS_ERR_PEC             = 2,   /* PEC-15 mismatch from LTC6812 */
    BMS_ERR_SPI             = 3,
    BMS_ERR_I2C             = 4,
    BMS_ERR_INVALID_ARG     = 5,
    BMS_ERR_FORBIDDEN       = 6,   /* operation not permitted on this chain */
    BMS_ERR_CONFIG_INVALID  = 7,
    BMS_ERR_FLASH           = 8,
    BMS_ERR_NOT_SUPPORTED   = 9,
    BMS_ERR_STALE           = 10,  /* measurement data is stale */
} BmsResult;

/* ── Measurement validity ─────────────────────────────────────────────────── */
typedef enum {
    MEAS_VALID   = 0,
    MEAS_INVALID = 1,   /* not yet acquired or stale */
    MEAS_ERROR   = 2,   /* acquisition error (PEC, SPI, etc.) */
} MeasValidity;

/* ── Cell voltage snapshot ────────────────────────────────────────────────── */
typedef struct {
    uint16_t     mv[TOTAL_CELL_COUNT]; /* raw cell voltages in mV */
    bool         valid[TOTAL_CELL_COUNT];
    uint32_t     timestamp_ms;
    MeasValidity overall;
} CellSnapshot;

/* ── Temperature snapshot ─────────────────────────────────────────────────── */
/* Temperatures stored as °C × 10 (int16_t). 0x8000 = INVALID sentinel. */
#define TEMP_INVALID_CX10  ((int16_t)0x8000)

typedef struct {
    int16_t      cx10[TOTAL_TEMP_COUNT];    /* °C × 10 or TEMP_INVALID_CX10 */
    uint16_t     raw_mv[TOTAL_TEMP_COUNT];  /* raw C-input voltage (mV) — the
                                             * value fed to the Enepaq table;
                                             * exposed for temp-topology debug */
    bool         valid[TOTAL_TEMP_COUNT];
    uint32_t     timestamp_ms;
    MeasValidity overall;
} TempSnapshot;

/* ── Pack-level measurements ──────────────────────────────────────────────── */
typedef struct {
    int32_t      vbat_mv;       /* battery terminal voltage via ISL28022; INT32_MIN if invalid */
    int32_t      vpack_mv;      /* load-side voltage via PA1 ADC;         INT32_MIN if invalid */
    int32_t      i_batt_ma;     /* positive = discharge */
    bool         vbat_valid;
    bool         vpack_valid;
    bool         i_batt_valid;
    uint32_t     timestamp_ms;
} PackMeasurement;

/* ── Fault bitmaps ────────────────────────────────────────────────────────── */
/* Bit positions match protocol/fault_bits.yaml exactly.
 * Do not reorder; the bitmap is transmitted over protocol as-is. */
typedef enum {
    FAULT_BIT_CELL_OV                    = 0,   /* Cell OV hard — blocks all */
    FAULT_BIT_CELL_UV                    = 1,   /* Cell UV hard — blocks discharge+master_ok */
    FAULT_BIT_CELL_OV_SOFT               = 2,   /* Cell OV soft — warning only */
    FAULT_BIT_CELL_UV_SOFT               = 3,   /* Cell UV soft — warning only */
    FAULT_BIT_CELL_READ_INVALID          = 4,   /* PEC/stale cell data — blocks all */
    FAULT_BIT_CELL_OPENWIRE              = 5,   /* Open-wire on cell — blocks all */
    FAULT_BIT_TEMP_OVER_CHARGE           = 6,   /* Temp > charge hard limit — blocks charge */
    FAULT_BIT_TEMP_OVER_DISCHARGE        = 7,   /* Temp > discharge hard limit — blocks discharge */
    FAULT_BIT_TEMP_OVER_ABS              = 8,   /* Temp > absolute max — blocks all */
    FAULT_BIT_TEMP_READ_INVALID          = 9,   /* PEC/stale temp data — blocks all */
    FAULT_BIT_TEMP_COVERAGE              = 10,  /* Required temp sensors invalid — blocks all */
    FAULT_BIT_VBAT_INVALID               = 11,  /* ISL28022 read fail — blocks all */
    FAULT_BIT_VPACK_INVALID              = 12,  /* ADC read fail — blocks discharge+master_ok */
    FAULT_BIT_ISOSPI_CELL                = 13,  /* CELL chain comms error — blocks all */
    FAULT_BIT_ISOSPI_TEMP                = 14,  /* TEMP chain comms error — blocks all */
    FAULT_BIT_I2C_ISL28022               = 15,  /* ISL28022 I2C error — blocks all */
    FAULT_BIT_WATCHDOG                   = 16,  /* IWDG fired — FATAL — blocks all */
    FAULT_BIT_CONFIG_INVALID             = 17,  /* Stored config invalid — blocks all */
    FAULT_BIT_OVERCURRENT                = 18,  /* |I| > overcurrent_hard_ma — blocks all */
    FAULT_BIT_BALANCE_TEMP_VIOLATION     = 19,  /* Temp exceeded balance inhibit — no blocks */
    FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT = 20,  /* DCC write to TEMP chain — FATAL */
    FAULT_BIT_TEMP_COLD_CHARGE           = 21,  /* Temp < cold charge limit — blocks charge */
    FAULT_BIT_TEMP_COLD_DISCHARGE        = 22,  /* Temp < cold discharge limit — warning */
    /* bits 23–63 reserved */
} FaultBit;

#define FAULT_MASK(bit)   ((uint64_t)1u << (bit))

/* Fatal: firmware bug or catastrophic hardware event → IWDG halt.
 * FAULT_BIT_WATCHDOG is deliberately NOT in this mask: it is set (latched)
 * after an IWDG-caused reset is detected at boot. Including it here would
 * re-enter the fatal halt path on every boot after a watchdog reset and
 * produce an endless reset loop. It still blocks all permission outputs
 * via the blocking masks below until explicitly cleared. */
#define FAULT_FATAL_MASK \
    (FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT))

/* Latching faults — must match `latching: true` bits in protocol/fault_bits.yaml.
 * A latching fault remains in the latched word (and keeps blocking its
 * permissions) until explicitly cleared via PKT_CLEAR_LATCHED_FAULTS.
 * Non-latching faults track their active condition only. */
#define FAULT_LATCHING_MASK \
    (FAULT_MASK(FAULT_BIT_CELL_OV)                    | \
     FAULT_MASK(FAULT_BIT_CELL_UV)                    | \
     FAULT_MASK(FAULT_BIT_CELL_OPENWIRE)              | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_CHARGE)           | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_DISCHARGE)        | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_ABS)              | \
     FAULT_MASK(FAULT_BIT_WATCHDOG)                   | \
     FAULT_MASK(FAULT_BIT_OVERCURRENT)                | \
     FAULT_MASK(FAULT_BIT_BALANCE_TEMP_VIOLATION)     | \
     FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT))

/* Blocks master_ok output */
#define FAULT_BLOCKS_MASTER_OK_MASK \
    (FAULT_MASK(FAULT_BIT_CELL_OV)           | \
     FAULT_MASK(FAULT_BIT_CELL_UV)           | \
     FAULT_MASK(FAULT_BIT_CELL_READ_INVALID) | \
     FAULT_MASK(FAULT_BIT_CELL_OPENWIRE)     | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_ABS)     | \
     FAULT_MASK(FAULT_BIT_TEMP_READ_INVALID) | \
     FAULT_MASK(FAULT_BIT_TEMP_COVERAGE)     | \
     FAULT_MASK(FAULT_BIT_VBAT_INVALID)      | \
     FAULT_MASK(FAULT_BIT_VPACK_INVALID)     | \
     FAULT_MASK(FAULT_BIT_ISOSPI_CELL)       | \
     FAULT_MASK(FAULT_BIT_ISOSPI_TEMP)       | \
     FAULT_MASK(FAULT_BIT_I2C_ISL28022)      | \
     FAULT_MASK(FAULT_BIT_WATCHDOG)          | \
     FAULT_MASK(FAULT_BIT_CONFIG_INVALID)    | \
     FAULT_MASK(FAULT_BIT_OVERCURRENT)       | \
     FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT))

/* Blocks discharge_perm output */
#define FAULT_BLOCKS_DISCHARGE_MASK \
    (FAULT_MASK(FAULT_BIT_CELL_OV)               | \
     FAULT_MASK(FAULT_BIT_CELL_UV)               | \
     FAULT_MASK(FAULT_BIT_CELL_READ_INVALID)     | \
     FAULT_MASK(FAULT_BIT_CELL_OPENWIRE)         | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_DISCHARGE)   | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_ABS)         | \
     FAULT_MASK(FAULT_BIT_TEMP_READ_INVALID)     | \
     FAULT_MASK(FAULT_BIT_TEMP_COVERAGE)         | \
     FAULT_MASK(FAULT_BIT_VBAT_INVALID)          | \
     FAULT_MASK(FAULT_BIT_VPACK_INVALID)         | \
     FAULT_MASK(FAULT_BIT_ISOSPI_CELL)           | \
     FAULT_MASK(FAULT_BIT_ISOSPI_TEMP)           | \
     FAULT_MASK(FAULT_BIT_I2C_ISL28022)          | \
     FAULT_MASK(FAULT_BIT_WATCHDOG)              | \
     FAULT_MASK(FAULT_BIT_CONFIG_INVALID)        | \
     FAULT_MASK(FAULT_BIT_OVERCURRENT)           | \
     FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT))

/* Blocks charge_perm output */
#define FAULT_BLOCKS_CHARGE_MASK \
    (FAULT_MASK(FAULT_BIT_CELL_OV)               | \
     FAULT_MASK(FAULT_BIT_CELL_READ_INVALID)     | \
     FAULT_MASK(FAULT_BIT_CELL_OPENWIRE)         | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_CHARGE)      | \
     FAULT_MASK(FAULT_BIT_TEMP_OVER_ABS)         | \
     FAULT_MASK(FAULT_BIT_TEMP_READ_INVALID)     | \
     FAULT_MASK(FAULT_BIT_TEMP_COVERAGE)         | \
     FAULT_MASK(FAULT_BIT_VBAT_INVALID)          | \
     FAULT_MASK(FAULT_BIT_ISOSPI_CELL)           | \
     FAULT_MASK(FAULT_BIT_ISOSPI_TEMP)           | \
     FAULT_MASK(FAULT_BIT_I2C_ISL28022)          | \
     FAULT_MASK(FAULT_BIT_WATCHDOG)              | \
     FAULT_MASK(FAULT_BIT_CONFIG_INVALID)        | \
     FAULT_MASK(FAULT_BIT_OVERCURRENT)           | \
     FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT) | \
     FAULT_MASK(FAULT_BIT_TEMP_COLD_CHARGE))

/* Blocks charger_safety output (same as charge for this design) */
#define FAULT_BLOCKS_CHARGER_SAFETY_MASK   FAULT_BLOCKS_CHARGE_MASK

/* Mask of faults that disable cell balancing */
#define FAULT_BLOCKS_BALANCING_MASK \
    (FAULT_BLOCKS_DISCHARGE_MASK                        | \
     FAULT_MASK(FAULT_BIT_BALANCE_TEMP_VIOLATION)       | \
     FAULT_MASK(FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT))

/* ── Permission request struct (passed to bms_outputs) ───────────────────── */
typedef struct {
    bool want_master_ok;
    bool want_discharge;
    bool want_charge;
    bool want_charger_safety;
} BmsPermissionRequest;

/* ── Output state bitmask (for protocol reporting) ───────────────────────── */
/* bit0=MasterOk, bit1=Discharge, bit2=Charge, bit3=ChargerSafety */
typedef uint8_t BmsOutputsBitmask;
#define OUTPUTS_BIT_MASTER_OK       (0x01u)
#define OUTPUTS_BIT_DISCHARGE       (0x02u)
#define OUTPUTS_BIT_CHARGE          (0x04u)
#define OUTPUTS_BIT_CHARGER_SAFETY  (0x08u)

/* ── Protocol error codes ─────────────────────────────────────────────────── */
typedef enum {
    PROTO_OK                    = 0x00,
    PROTO_ERR_UNKNOWN_PACKET    = 0x01,
    PROTO_ERR_BAD_LENGTH        = 0x02,
    PROTO_ERR_BAD_CRC           = 0x03,
    PROTO_ERR_BAD_SEQUENCE      = 0x04,
    PROTO_ERR_CONFIG_INVALID    = 0x05,
    PROTO_ERR_WRONG_TARGET      = 0x06,
    PROTO_ERR_PACKAGE_INVALID   = 0x07,
    PROTO_ERR_FLASH_FAIL        = 0x08,
    PROTO_ERR_BUSY              = 0x09,
    PROTO_ERR_NOT_SUPPORTED     = 0x0A,
    PROTO_ERR_BAD_STATE         = 0x0B,
    PROTO_ERR_VERSION_MISMATCH  = 0x0C,
    PROTO_ERR_INTERNAL          = 0x0D,
} ProtoError;
