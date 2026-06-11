# 02 — Safety Model

## 1. Safety Invariants

These must hold at all times regardless of state, configuration, or communication activity:

| ID | Invariant |
|---|---|
| SI-01 | All permission outputs default to DEASSERTED after reset |
| SI-02 | No measurement data older than `STALE_DATA_TIMEOUT_MS` may be used for permission decisions |
| SI-03 | A PEC error on any LTC6812 read invalidates the entire read; no partial data is used |
| SI-04 | TEMP-chain LTC6812 devices never receive DCC/balance writes |
| SI-05 | Invalid Vbat or invalid Vpack must independently block precharge completion |
| SI-06 | Only `bms_outputs` module writes permission GPIOs |
| SI-07 | Balancing is disabled whenever cell data is invalid, open-wire result is inconclusive, or state prohibits it |
| SI-08 | POWER_ENABLE is not released until all permission outputs are confirmed deasserted |
| SI-09 | Firmware rejects config with schema version mismatch, hardware profile mismatch, CRC failure, or unsafe thresholds |
| SI-10 | Any active fault with severity ≥ CRITICAL deasserts all permissions immediately |

---

## 2. Boot / Reset Defaults

On any reset (power-on, watchdog, software, pin reset):

1. `board_outputs` initializes — all permission GPIOs driven to safe (DEASSERTED) state before any other init.
2. Watchdog is configured and started before any sensor reads.
3. Config is loaded from flash and fully validated; if invalid, `FAULT_CONFIG_INVALID` is set and safe defaults are used.
4. Measurements are marked STALE/INVALID until first successful read completes.
5. State machine enters `STATE_BOOT`, transitions to `STATE_IDLE` after init, not to any active state.
6. No permission is asserted during boot sequence.

---

## 3. Measurement Validity Rules

A measurement result is **VALID** only if ALL of the following are true:

| Measurement | Validity Conditions |
|---|---|
| Cell voltage (single) | PEC matched, conversion complete (PLADC or timeout), value ≥ 0 and ≤ 5000 mV (raw range), age ≤ `STALE_DATA_TIMEOUT_MS` |
| Cell voltage (all, for permission) | All 75 channels valid, or channels not in `required_cell_mask` are excluded with explicit allow-missing policy |
| Temperature (single sensor) | PEC matched, voltage within Enepaq sensor operating range, V-T conversion did not return out-of-range, age ≤ `STALE_DATA_TIMEOUT_MS` |
| Temperature (for permission) | All sensors in `required_temp_mask` are valid |
| Vbat (ISL28022) | I2C ACK received, ready bit set, value ≥ MIN_VBAT, age ≤ `STALE_DATA_TIMEOUT_MS` |
| Vpack (ADC) | ADC conversion complete, value within calibrated operating range, age ≤ `STALE_DATA_TIMEOUT_MS` |
| Current | I2C ACK received, shunt register read valid, age ≤ `STALE_DATA_TIMEOUT_MS` |

**If any required measurement is invalid:**
- Set corresponding fault bit
- Block any permission that depends on that measurement
- Do NOT use last-known-good value for safety decisions

`STALE_DATA_TIMEOUT_MS` default: 500 ms. Configurable.

---

## 4. Active vs Latched Faults

| Type | Behaviour | Clear Method |
|---|---|---|
| **Active fault** | Set while condition is present; auto-clears when condition resolves | Automatic on resolution |
| **Latched fault** | Set on first occurrence; remains set until explicitly cleared even if condition resolves | Explicit clear via protocol command (GET_CLEAR_FAULTS), requires condition resolved first |

All faults start as active. Faults listed with `latching: true` in `fault_bits.yaml` become latched. Latched faults must be explicitly cleared by an authorized protocol command. Latched faults with active underlying condition cannot be cleared.

---

## 5. Fault Severity Levels

| Level | Value | Meaning | Permission Effect |
|---|---|---|---|
| INFO | 0 | Informational; no immediate action | None |
| WARNING | 1 | Degraded operation; log and monitor | None by default; configurable |
| ERROR | 2 | Fault condition; restrict operation | Block affected permission |
| CRITICAL | 3 | Safety fault; immediate deassert all | Deassert ALL permissions |
| FATAL | 4 | Unrecoverable; attempt controlled shutdown | Deassert all; release POWER_ENABLE |

---

## 6. Fault Bits and Meanings

Stored as two 64-bit words: `active_faults` and `latched_faults`.

| Bit | Name | Severity | Latching | Source | Condition |
|---|---|---|---|---|---|
| 0 | FAULT_CELL_OV | CRITICAL | YES | bms_measurements | Any cell voltage > `cell_ov_hard_mv` |
| 1 | FAULT_CELL_UV | CRITICAL | YES | bms_measurements | Any cell in required_mask < `cell_uv_hard_mv` |
| 2 | FAULT_CELL_OV_SOFT | WARNING | NO | bms_measurements | Any cell > `cell_ov_soft_mv` |
| 3 | FAULT_CELL_UV_SOFT | WARNING | NO | bms_measurements | Any cell < `cell_uv_soft_mv` |
| 4 | FAULT_CELL_READ_INVALID | ERROR | NO | bms_measurements | PEC error or stale data on CELL chain |
| 5 | FAULT_CELL_OPENWIRE | ERROR | YES | bms_diagnostics | Open-wire detected on any required cell |
| 6 | FAULT_TEMP_OVER_CHARGE | CRITICAL | YES | bms_measurements | Any required temp sensor > `temp_charge_hard_c` during charge |
| 7 | FAULT_TEMP_OVER_DISCHARGE | CRITICAL | YES | bms_measurements | Any required temp sensor > `temp_discharge_hard_c` during discharge |
| 8 | FAULT_TEMP_OVER_ABS | CRITICAL | YES | bms_measurements | Any required temp sensor > `temp_hard_abs_c` |
| 9 | FAULT_TEMP_READ_INVALID | ERROR | NO | bms_measurements | PEC error, stale, or out-of-range on TEMP chain |
| 10 | FAULT_TEMP_COVERAGE | ERROR | NO | bms_measurements | One or more required_temp_mask sensors invalid |
| 11 | FAULT_VBAT_INVALID | ERROR | NO | bms_measurements | ISL28022 read failed or stale |
| 12 | FAULT_VPACK_INVALID | ERROR | NO | bms_measurements | PA1 ADC invalid or stale |
| 13 | FAULT_PRECHARGE_TIMEOUT | ERROR | YES | bms_state | Precharge did not complete within `precharge_timeout_ms` |
| 14 | FAULT_PRECHARGE_DELTA | ERROR | YES | bms_state | Vpack/Vbat ratio outside expected after precharge |
| 15 | FAULT_ISOSPI_CELL | ERROR | NO | ltc6812 driver | Persistent PEC errors on CELL chain (>N consecutive) |
| 16 | FAULT_ISOSPI_TEMP | ERROR | NO | ltc6812 driver | Persistent PEC errors on TEMP chain |
| 17 | FAULT_I2C_ISL28022 | ERROR | NO | isl28022 driver | I2C NACK or timeout on ISL28022 |
| 18 | FAULT_WATCHDOG | FATAL | YES | bms_main_loop | IWDG reset detected at boot (latched until explicitly cleared) |
| 19 | FAULT_CONFIG_INVALID | ERROR | NO | bms_config | Stored config failed validation at boot |
| 20 | FAULT_OVERCURRENT | CRITICAL | YES | bms_measurements | |I_batt| > `overcurrent_hard_a` |
| 21 | FAULT_BALANCE_TEMP_VIOLATION | ERROR | YES | bms_balance | Balancing inhibited by temperature condition |
| 22 | FAULT_TEMP_CHAIN_BALANCE_ATTEMPT | FATAL | YES | ltc6812 driver | DCC write attempted to TEMP chain (firmware error) |
| 23–63 | Reserved | — | — | — | — |

> **Note:** Exact fault set may expand during implementation. All fault bits are named in `protocol/fault_bits.yaml`.

---

## 7. Permission Gating Matrix

`1` = this fault/condition blocks this permission when active. Blank = no effect.

| Fault / Condition | MASTER_OK | DISCHARGE_PERM | CHARGE_PERM | CHARGER_SAFETY |
|---|---|---|---|---|
| FAULT_CELL_OV | 1 | 1 | 1 | 1 |
| FAULT_CELL_UV | 1 | 1 | | |
| FAULT_CELL_READ_INVALID | 1 | 1 | 1 | 1 |
| FAULT_CELL_OPENWIRE | 1 | 1 | 1 | 1 |
| FAULT_TEMP_OVER_CHARGE | | | 1 | 1 |
| FAULT_TEMP_OVER_DISCHARGE | | 1 | | |
| FAULT_TEMP_OVER_ABS | 1 | 1 | 1 | 1 |
| FAULT_TEMP_READ_INVALID | 1 | 1 | 1 | 1 |
| FAULT_TEMP_COVERAGE | 1 | 1 | 1 | 1 |
| FAULT_VBAT_INVALID | 1 | 1 | 1 | 1 |
| FAULT_VPACK_INVALID (discharge/precharge) | 1 | 1 | | |
| FAULT_PRECHARGE_TIMEOUT | 1 | | | |
| FAULT_OVERCURRENT | 1 | 1 | 1 | 1 |
| FAULT_CONFIG_INVALID | 1 | 1 | 1 | 1 |
| FAULT_I2C_ISL28022 | 1 | 1 | 1 | 1 |
| Any CRITICAL fault | 1 | 1 | 1 | 1 |
| Any FATAL fault | 1 | 1 | 1 | 1 |
| STATE != DISCHARGING | | 1 | | |
| STATE != CHARGING | | | 1 | 1 |
| Precharge not complete | 1 | 1 | | |

MASTER_OK is deasserted if any condition in its column is present. Permissions are AND-gated: all blocking conditions must be false for the permission to be assertable.

---

## 8. Balancing Gating Rules

Balancing is **DISABLED** (all DCC bits cleared) if any of the following are true:

1. `FAULT_CELL_READ_INVALID` is active
2. `FAULT_CELL_OPENWIRE` is active and open-wire check is enabled in config
3. `FAULT_CELL_OV` is active
4. `FAULT_TEMP_OVER_ABS` is active
5. `FAULT_BALANCE_TEMP_VIOLATION` is active
6. State is not `STATE_BALANCING` or `STATE_DISCHARGING` (policy: only balance when permitted by state)
7. `FAULT_CONFIG_INVALID` is active
8. Any bit set in the computed balance mask falls outside `balance_allowed_mask`

In addition:
- A cell may only be balanced if its local cell index maps to a set bit in `balance_allowed_mask`
- Balance must target CELL chain only; the TEMP chain must never receive DCC writes

---

## 9. TEMP-Chain Restrictions

| Rule | Enforcement Layer |
|---|---|
| No DCC/balance writes to TEMP chain | `ltc6812` driver: `chain_id` parameter; FATAL fault if violated |
| No ADCV command to TEMP chain | `ltc6812` driver: ADCV only callable with `CHAIN_CELL` |
| S outputs cleared after every measurement | `bms_measurements`: always in both success and error paths |
| TEMP chain config writes limited to S-output bits only | `bms_measurements`: constructs CFGRA with only GPIO/S bits; DCC bits hardcoded 0 |

The `ltc6812` driver has a runtime assertion: if `chain_id == CHAIN_TEMP` and DCC bits are non-zero in the config write, log `FAULT_TEMP_CHAIN_BALANCE_ATTEMPT` and do NOT transmit the write.

---

## 10. Precharge / Contact Validation Logic

```
STATE: PRECHARGING
  │
  ├── Vbat valid?   No → FAULT_VBAT_INVALID → abort
  ├── Vpack valid?  No → FAULT_VPACK_INVALID → abort
  │
  ├── Timer running (max: precharge_timeout_ms)
  │     exceeded → FAULT_PRECHARGE_TIMEOUT → abort
  │
  ├── Check: Vpack ≥ (Vbat × precharge_pct / 100)
  │     Yes → precharge complete
  │     No  → continue waiting
  │
  └── On completion:
        Verify: |Vpack - Vbat| / Vbat < PRECHARGE_DELTA_MAX_PCT
          FAIL → FAULT_PRECHARGE_DELTA → do not grant discharge permission
          PASS → transition STATE_CLOSING_MAIN → STATE_DISCHARGING
```

Both Vbat and Vpack must be fresh (within `STALE_DATA_TIMEOUT_MS`) at the moment of the completion check. A stale reading is treated as invalid.

---

## 11. Stale Data Behaviour

If any measurement timestamp exceeds `STALE_DATA_TIMEOUT_MS` (default 500 ms):

1. The measurement result is marked `VALIDITY_STALE`
2. The corresponding measurement fault bit is set
3. Any permission gated on that measurement is blocked
4. The stale status is reported in telemetry

The firmware does **not** extrapolate, interpolate, or use the last known good value for safety decisions after the stale timeout.

---

## 12. Watchdog / Fatal / Error Behaviour

**IWDG (Independent Watchdog):**
- Configured at boot; must be kicked every `WATCHDOG_TIMEOUT_MS` (default 500 ms)
- Kicked by the main measurement/control loop
- If the loop stalls, IWDG resets the MCU
- At reset, firmware logs the reset cause (via RCC reset flags) to retained SRAM or ring buffer in flash for post-mortem

**Software Fatal:**
- `bms_fatal()` function: logs fault, deasserts all permissions via `bms_outputs`, stops balancing, then either halts or triggers IWDG
- No return from fatal path

**Error handling:**
- PEC errors: retry up to `LTC6812_MAX_PEC_RETRIES` (default 3) before setting fault
- I2C NACK: retry up to `I2C_MAX_RETRIES` (default 3) before setting fault
- All errors logged via ring buffer available to `GET_DIAGNOSTICS_SUMMARY`

---

## 13. Config-Invalid Behaviour

If config fails validation at boot (wrong magic, wrong schema version, CRC mismatch, unsafe thresholds, wrong hardware profile):

1. `FAULT_CONFIG_INVALID` is set (latched)
2. Firmware loads compile-time safe defaults (see `bms_config.c`)
3. All permissions remain blocked (MASTER_OK deasserted)
4. BMS enters `STATE_CONFIG_ERROR` — monitoring only, no actuation
5. Desktop tool can read faults, connect, read capabilities, and write a valid config to resolve
6. After valid config is written via `STORE_CONFIG`, firmware requests soft reset to re-initialize from stored config

**Safe defaults (firmware hard-coded):**
- No thresholds set (all permissions blocked by default until config loaded)
- All masks = 0x00 (no cells/sensors required, no balancing — fully restrictive)
- All timeouts = maximum conservative values

---

## 14. Communication Loss Behaviour

**USB/UART protocol timeout:** if no valid request received for `PROTOCOL_IDLE_TIMEOUT_MS`:
- No permission change (communication loss is not a safety fault by default)
- Log event to diagnostic ring buffer
- CAN-watchdog policy: **OPEN QUESTION** — should CAN message absence trip a fault? Define timeout threshold and whether it affects permissions.

**CAN timeout (if configured):**
- If `can_watchdog_timeout_ms` > 0 in config, absence of CAN heartbeat from external master for that duration sets `FAULT_CAN_WATCHDOG` (currently defined but policy is configurable)
- Default: disabled (0 = no timeout)

---

## 15. What Must Fail Safe

| Scenario | Required Behaviour |
|---|---|
| MCU reset (any cause) | All permissions deasserted at first GPIO init |
| Config unreadable | Permissions blocked; monitoring only |
| isoSPI CELL chain non-responsive | `FAULT_CELL_READ_INVALID`; all permissions blocked |
| isoSPI TEMP chain non-responsive | `FAULT_TEMP_READ_INVALID`; permissions blocked if temp coverage required |
| ISL28022 I2C failure | `FAULT_I2C_ISL28022`; Vbat/current invalid; permissions blocked |
| TEMP S outputs left asserted after fault | Must clear S outputs in all fault/abort paths; hardware damage risk otherwise |
| Balance mask write to TEMP chain | Must never transmit; FATAL fault logged |
| Open-wire detected | Block balancing; block discharge if cell in required mask |
| Firmware package wrong hardware ID | Bootloader refuses flash |
| Incomplete firmware write (power loss) | Bootloader detects incomplete update; stays in bootloader; waits for retry |
