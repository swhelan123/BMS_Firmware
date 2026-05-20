# 00 — System Purpose

## 1. Purpose

This BMS monitors a 75-cell lithium-ion battery stack, enforces charge/discharge safety through permission outputs, and reports state to an external host over CAN and USB/UART. It is designed for a supervised application where a downstream shutdown circuit interprets BMS permission signals to open or close contactors.

---

## 2. Safety Boundary

The BMS **does not** directly drive AIRs (Automotive Interrupt Relays), precharge relays, or any high-side switching element. It asserts or deasserts logic-level permission outputs that feed into a downstream shutdown/interlock circuit. That circuit makes the final contactor decision.

This boundary is intentional and must never be blurred in firmware, configuration, or tooling.

---

## 3. What the BMS Does

| Function | Description |
|---|---|
| Cell voltage measurement | 5 × LTC6812 CELL chain; 75 channels at 100 µV resolution |
| Temperature measurement | 5 × LTC6812 TEMP chain; 75 Enepaq sensor voltage channels |
| Pack voltage measurement | ISL28022 (Vbat, battery side) + STM32 ADC PA1 (Vpack, load side) |
| Current measurement | ISL28022 shunt current measurement |
| Cell balancing | Passive balancing via CELL-chain DCC bits (per-cell, gated) |
| Fault detection | Cell OV/UV, temperature over-range, open-wire, communication errors |
| Permission management | Assert/deassert MasterOk, DischargePermission, ChargePermission, ChargerSafety |
| Precharge validation | Compare Vbat vs Vpack; validate ratio before permission |
| Communication | USB/UART debug/config protocol; CAN telemetry (hardware pins wired; firmware stub only — not yet implemented) |
| Configuration | Persistent versioned config stored in flash |
| Update | ST-Link development flash; bootloader-based production update |

---

## 4. What the BMS Does Not Directly Control

- AIR coils or precharge relay coils (downstream circuit responsibility)
- Load-side contactor sequencing (downstream circuit responsibility)
- Charger enable/disable beyond ChargerSafety/ChargePermission signal level
- High-voltage switching of any kind
- External balancing hardware (passive only, via LTC6812 DCC)

---

## 5. Subsystem Relationships

```
         ┌──────────────────────────────────┐
         │          BMS Firmware            │
         │                                  │
         │  Measurements ──► Faults ──► Outputs
         │       │                          │
         │  Config (flash) ◄─── Protocol    │
         │       │                          │
         │  State Machine ────────────────► GPIO permissions
         └──────────────────────────────────┘
              │              │           │
         isoSPI           I2C/ADC       CAN / USB-UART
              │              │           │
         LTC6812 ×10      ISL28022    Desktop Tool /
         (CELL+TEMP)      + PA1 ADC   External BMS master
```

### Sensing → Faults
Raw measurements are validated (range, freshness, PEC) before entering the fault evaluator. Invalid readings conservatively produce faults.

### Faults → Outputs
The output module reads the consolidated fault bitmap and required state to decide which permissions to assert. No other module writes GPIO directly.

### Protocol → Config
The protocol layer deserializes config packets and passes fully-validated config blobs to `bms_config`. Partial or malformed configs are rejected before they touch stored state.

### Bootloader
The bootloader is a separate MCU image. It handles firmware package validation and flash programming. The application requests bootloader entry through a flag in retained memory; the bootloader exposes its own identity. The desktop tool treats bootloader-mode and application-mode as distinct states.

---

## 6. Non-Goals

- The BMS is not a BMS master controller that directly sequences precharge (it provides signals to a circuit that does).
- The BMS does not implement cell chemistry SOC estimation in v1. SoC is a future feature.
- The BMS does not authenticate firmware packages cryptographically (integrity-checked, not signed).
- The BMS is not a CAN-primary safety authority; CAN/USB are telemetry/configuration paths.
- The TEMP-chain LTC6812 devices are not cell monitors and must never be used as such.

---

## 7. High-Level Data and Control Flow

```
POWER-ON
  │
  ▼
board_clock / board_pins init
  │
  ▼
board_outputs → deassert all permissions (safe default)
  │
  ▼
board_flash → load + validate config
  │  invalid ──► use safe defaults, set CONFIG_INVALID fault
  ▼
bms_state: BOOT → IDLE
  │
  ▼
Main loop (10 ms tick nominal):
  ├─ bms_measurements:
  │    ├─ ltc6812 CELL chain read (wake → ADCV → RDCV × 5 regs × 5 ICs)
  │    ├─ ltc6812 TEMP chain read (wake → bias enable → settle → ADCV → RDCV → bias off)
  │    ├─ isl28022 read (Vbat, I_shunt)
  │    └─ board_adc read (Vpack)
  │
  ├─ bms_faults:
  │    ├─ evaluate cell OV/UV/open-wire against thresholds
  │    ├─ evaluate temperature limits
  │    ├─ evaluate precharge validity
  │    ├─ evaluate stale data
  │    └─ update active fault bitmap; latch faults that require explicit clear
  │
  ├─ bms_state:
  │    ├─ evaluate state transitions
  │    └─ request desired permission set from bms_outputs
  │
  ├─ bms_outputs:
  │    └─ apply permission set, gated by fault bitmap, to GPIO
  │
  ├─ bms_balance:
  │    └─ compute balance mask (CELL chain only, gated by faults/config/state)
  │
  └─ bms_protocol / bms_can:
       └─ service pending requests / emit telemetry
```
