"""bms_defs.py — single source of truth for BMS fault bits and state names.

Mirrors protocol/fault_bits.yaml and firmware bms_types.h. Every tool layer
(CLI, GUI, fake target, tests) imports from here — never redefine these
tables locally. tool/tests/test_protocol_sync.py asserts this module stays
in lock-step with the YAML contract and the firmware header.
"""

# ── Fault bit positions (must match protocol/fault_bits.yaml) ─────────────────
FAULT_BIT_CELL_OV                    = 0
FAULT_BIT_CELL_UV                    = 1
FAULT_BIT_CELL_OV_SOFT               = 2
FAULT_BIT_CELL_UV_SOFT               = 3
FAULT_BIT_CELL_READ_INVALID          = 4
FAULT_BIT_CELL_OPENWIRE              = 5
FAULT_BIT_TEMP_OVER_CHARGE           = 6
FAULT_BIT_TEMP_OVER_DISCHARGE        = 7
FAULT_BIT_TEMP_OVER_ABS              = 8
FAULT_BIT_TEMP_READ_INVALID          = 9
FAULT_BIT_TEMP_COVERAGE              = 10
FAULT_BIT_VBAT_INVALID               = 11
FAULT_BIT_VPACK_INVALID              = 12
FAULT_BIT_ISOSPI_CELL                = 13
FAULT_BIT_ISOSPI_TEMP                = 14
FAULT_BIT_I2C_ISL28022               = 15
FAULT_BIT_WATCHDOG                   = 16
FAULT_BIT_CONFIG_INVALID             = 17
FAULT_BIT_OVERCURRENT                = 18
FAULT_BIT_BALANCE_TEMP_VIOLATION     = 19
FAULT_BIT_TEMP_CHAIN_BALANCE_ATTEMPT = 20
FAULT_BIT_TEMP_COLD_CHARGE           = 21
FAULT_BIT_TEMP_COLD_DISCHARGE        = 22

# Index = bit position. Names match fault_bits.yaml minus the FAULT_ prefix.
FAULT_NAMES = [
    "CELL_OV", "CELL_UV", "CELL_OV_SOFT", "CELL_UV_SOFT",
    "CELL_READ_INVALID", "CELL_OPENWIRE", "TEMP_OVER_CHARGE", "TEMP_OVER_DISCHARGE",
    "TEMP_OVER_ABS", "TEMP_READ_INVALID", "TEMP_COVERAGE", "VBAT_INVALID",
    "VPACK_INVALID", "ISOSPI_CELL", "ISOSPI_TEMP", "I2C_ISL28022",
    "WATCHDOG", "CONFIG_INVALID", "OVERCURRENT", "BALANCE_TEMP_VIOLATION",
    "TEMP_CHAIN_BALANCE_ATTEMPT", "TEMP_COLD_CHARGE", "TEMP_COLD_DISCHARGE",
]


def fault_name(bit: int) -> str:
    """Human-readable name for a fault bit (BIT_n for reserved bits)."""
    return FAULT_NAMES[bit] if 0 <= bit < len(FAULT_NAMES) else f"BIT_{bit}"


def fault_names_from_mask(mask: int) -> list:
    """List of fault names for every set bit in a 64-bit fault bitmap."""
    return [fault_name(b) for b in range(64) if mask & (1 << b)]


# ── BMS state machine values (must match firmware BmsState enum) ──────────────
BMS_STATE_INIT      = 0
BMS_STATE_STANDBY   = 1
BMS_STATE_DISCHARGE = 2
BMS_STATE_CHARGE    = 3
BMS_STATE_FAULT     = 4
BMS_STATE_SHUTDOWN  = 5

STATE_NAMES = {
    BMS_STATE_INIT:      "INIT",
    BMS_STATE_STANDBY:   "STANDBY",
    BMS_STATE_DISCHARGE: "DISCHARGE",
    BMS_STATE_CHARGE:    "CHARGE",
    BMS_STATE_FAULT:     "FAULT",
    BMS_STATE_SHUTDOWN:  "SHUTDOWN",
}


def state_name(value: int) -> str:
    return STATE_NAMES.get(value, f"UNKNOWN({value})")
