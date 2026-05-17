# Bench Safety Checklist — BMS First-Flash Session

> **Use this checklist before and during every first-hardware bench session.**
> Work through each section in order. Do not skip steps. Mark each item before proceeding.

---

## Section 1 — Before Applying Power

### 1.1 Board Inspection

- [ ] Board has been visually inspected for obvious assembly defects (bridges, lifted pads,
      missing components, reversed polarities on polarised capacitors).
- [ ] No conductive debris or swarf on the PCB surface.
- [ ] All connectors are correctly seated and keyed in the correct direction.
- [ ] The board is mounted on a non-conductive surface or anti-static mat. Not resting on
      metal tooling.

### 1.2 High-Voltage and Accumulator

- [ ] **No full accumulator is connected.** No battery stack. No HV bus.
- [ ] Any HV connectors on the board are physically isolated (covered, unplugged, or have
      no cable attached).
- [ ] The contactor actuation path is not connected to the board if it can be avoided.
      If it is connected, verify that downstream devices cannot actuate.
- [ ] No load is connected to the pack-side or load-side terminals.
- [ ] No inverter, motor controller, or vehicle harness is connected.
- [ ] No charger is connected.

### 1.3 Bench Supply Setup

- [ ] Bench supply is set to **12V** into the power-supply subsystem
- [ ] Bench supply output is **off** before connecting to the board.
- [ ] Supply polarity confirmed by DMM before connecting.
- [ ] GND reference is shared between bench supply, ST-Link, and UART adapter.

### 1.4 ST-Link Wiring

- [ ] ST-Link connected to: SWDIO → PA13, SWDCK → PA14, GND → board GND.
- [ ] ST-Link does NOT supply power (VCC pin disconnected or set to sense-only) unless
      it is the sole power source for this session.
- [ ] NRST pin connected if hardware reset is required (optional but recommended).
- [ ] SWD cable is short (< 20 cm recommended for 4 MHz). No loose connections.
- [ ] `STM32_Programmer_CLI -l` confirms the ST-Link is enumerated on the host.

### 1.5 UART Wiring

- [ ] PA2 (UART2_TX) → CP2104 RX (or external UART adapter RX).
- [ ] PA3 (UART2_RX) → CP2104 TX (or external UART adapter TX).
- [ ] GND shared. Note: TX/RX are named from the MCU perspective — cross them to the
      adapter.
- [ ] Baud rate set to **115200, 8N1, no flow control** in the terminal / bmsctl.
- [ ] UART adapter is recognised by the host: `ls /dev/tty.usbserial-* /dev/tty.SLAB_*`.

### 1.6 BOOT0 / Boot Mode

- [ ] BOOT0 is pulled **LOW** on the board for normal boot (boot from flash).
      If BOOT0 is HIGH at reset, the MCU enters the factory system bootloader — this will
      appear as no BMS UART output.
- [ ] If you need to use the system bootloader (e.g., USB DFU), this is a deliberate action;
      confirm and document it separately.

### 1.7 Ground Reference

- [ ] The STM32 GND, bench supply GND, UART adapter GND, and any oscilloscope probe GND
      are all connected to the same reference node.
- [ ] Oscilloscope probe GND clip is not accidentally connected to an isolated HV node
      (not applicable if HV is not present — double-check anyway).

---

## Section 2 — After Power-On, Before Issuing Commands

Apply power. Wait 1 second. Perform these checks before sending any bmsctl commands.

### 2.1 Supply Rails

- [ ] Measure +3.3V at the MCU VDD pin. Expected: 3.25–3.35 V.
- [ ] Current draw from bench supply is plausible. Expected idle current for STM32F303 + 
      CP2104 + LTC6820 (without isoSPI chain or LTC6812 VCC): typically 20–60 mA.
      If > 150 mA immediately after power-on: power off and investigate.

### 2.2 Permission Output Levels (measure before software checks)

- [ ] Measure PB10 (DISCHARGE_PERM) with DMM. Record actual voltage: _____ V.
- [ ] Measure PB11 (MASTER_OK) with DMM. Record actual voltage: _____ V.
- [ ] Measure PB0 (CHARGE_PERM) with DMM. Record actual voltage: _____ V.
- [ ] Measure PB2 (CHARGER_SAFETY) with DMM. Record actual voltage: _____ V.
- [ ] Confirm that the levels on PB10/PB11/PB0/PB2 are at the **inactive / safe state**
      for the downstream circuit. The exact inactive voltage (LOW or HIGH) depends on the
      output polarity (open question HV-3). Record and confirm.

> **If any permission output is at its active level immediately after power-on: power off.
> Do not proceed until `board_outputs_init_safe()` is verified to be working correctly.**

### 2.3 Power Enable

- [ ] PB5 (POWER_ENABLE): note its state. If the board has a power-latch circuit, this pin
      must be HIGH to hold the board alive. If it is LOW and the board is still powered
      from the bench supply, this is expected — the bench supply holds power, not the latch.

### 2.4 CS Line Idle State

- [ ] Measure PA4 (CS_CELL) with DMM. Expected: **HIGH (3.3V)** at idle.
- [ ] Measure PB12 (CS_TEMP) with DMM. Expected: **HIGH (3.3V)** at idle.
- [ ] If either CS is LOW: SPI or GPIO initialisation failed. Do not proceed with isoSPI
      commands — a stuck-low CS may cause unintended LTC6820 / LTC6812 activity.

### 2.5 TEMP Sensor Bias

- [ ] Confirm that no TEMP chain sensor-bias S-outputs are active at idle.
      (Without an LTC6812 chain connected this cannot be measured; note for when chain is
      connected.)
- [ ] The firmware must not assert any S-output on the TEMP chain outside of a measurement
      cycle. Verify by confirming no current draw path through sensor bias resistors at idle.

### 2.6 No Balancing Active

- [ ] Cell balancing DCC outputs must not be active at first boot. MCU permission outputs are set inactive by board_outputs_init_safe(). LTC6812 DCC balancing bits should remain zero at startup and must be explicitly cleared through the CELL-chain balance disable path once the CELL chain is available.
- [ ] (With CELL chain connected) Measure any balancing resistor: no current should flow
      at idle.

---

## Section 3 — Command Sequence

Run commands in this order. Do not skip steps. Do not proceed to the next step if the
current step fails.

```
SPORT="--serial /dev/tty.usbserial-XXXX"   # replace with your port
```

| # | Command | Pass Condition | Stop if Fail? |
|---|---|---|---|
| 1 | `bmsctl connect $SPORT` | `mode: BMS_APP`, exit 0 | Yes |
| 2 | `bmsctl diagnostics $SPORT` | `uptime_ms > 0` | Yes |
| 3 | `bmsctl diag gpio $SPORT` | `cs_cell=1, cs_temp=1` | Yes |
| 4 | `bmsctl diag outputs $SPORT` | `logical_state: 0x00` | **Yes — stop if any permission active** |
| 5 | `bmsctl probe cell-chain $SPORT` | 5 ICs respond (with chain) | No (skip if chain absent) |
| 6 | `bmsctl probe temp-chain $SPORT` | 5 ICs respond (with chain) | No (skip if chain absent) |
| 7 | `bmsctl probe isl $SPORT` | `status: OK` (with ISL) | No (skip if ISL absent) |
| 8 | `bmsctl read vpack-raw $SPORT` | `status: OK` | No |
| 9 | `bmsctl measure cells $SPORT` | All 75 valid (with chain) | — |
| 10 | `bmsctl measure temps $SPORT` | All 75 valid (with chain) | — |
| 11 | `bmsctl measure power $SPORT` | Vbat/Vpack/I valid (with ISL) | — |

---

## Section 4 — Explicit Prohibitions

These actions must not occur during or after the first-flash session until the relevant
open questions (from `docs/01_hardware_contract.md`) have been resolved:

- **Do not connect a full battery accumulator** until all fault thresholds have been
  reviewed and the config validated.
- **Do not enable cell balancing** (`balance disable-all` is the only safe balancing
  command during bring-up). Do not write DCC bits to any LTC6812.
- **Do not treat measurement scaling as calibrated.** All Vbat/Vpack/I readings require
  calibration constants that depend on the actual resistor divider, shunt resistance, and
  VREF source (open questions HV-5, HV-6, HV-13). Treat raw values as uncalibrated until
  confirmed.
- **Do not rely on open-wire detection as a safety sign-off.** The ADOW scan tests cell
  wire continuity but does not validate measurement accuracy.
- **Do not assert any permission outputs** (MASTER_OK, DISCHARGE_PERM, CHARGE_PERM,
  CHARGER_SAFETY) until the active polarity (HV-3) has been confirmed from the schematic.
  There is no bmsctl command that asserts permissions during a bring-up session (the only
  assertions are through the fault/state machine in normal operation).
- **Do not connect vehicle harness, charger, or inverter** during this session.

---

*See also: [first_flash_guide.md](first_flash_guide.md),
[uart_smoke_test.md](uart_smoke_test.md),
[01_hardware_contract.md](01_hardware_contract.md),
[02_safety_model.md](02_safety_model.md)*
