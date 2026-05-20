# 01 — Hardware Contract

## 1. Pin / Signal Table

| Pin | Signal | Direction | Function | Notes |
|---|---|---|---|---|
| PA4 | CS_CELL | OUT | isoSPI chip-select for CELL chain LTC6820 | Active-low; idle HIGH; only one CS active at a time |
| PB12 | CS_TEMP | OUT | isoSPI chip-select for TEMP chain LTC6820 | Active-low; idle HIGH; only one CS active at a time |
| PA5 | SPI1_SCK | OUT | SPI clock shared by both chains | SPI Mode 3 (CPOL=1, CPHA=1) |
| PA6 | SPI1_MISO | IN | SPI MISO shared by both chains | AF5 |
| PA7 | SPI1_MOSI | OUT | SPI MOSI shared by both chains | AF5 |
| PA1 | VPACK_ADC | IN (analog) | ADC1_IN2; load-side / precharge-bus voltage | Scaled analogue input; see calibration in config |
| PA9 | I2C2_SCL | OUT | I2C2 clock to ISL28022 | AF4; 100 kHz standard mode (bring-up); 4.75 kΩ external pull-up |
| PA10 | I2C2_SDA | BIDIR | I2C2 data to ISL28022 | AF4 |
| PB11 | MASTER_OK | OUT | MasterOk / multipurpose permission into shutdown logic | Not a direct relay driver; MCU HIGH = asserted (see §11) |
| PB10 | DISCHARGE_PERM | OUT | DischargePermission into shutdown logic | Not a direct relay driver |
| PB0 | CHARGE_PERM | OUT | ChargePermission | |
| PB2 | CHARGER_SAFETY | OUT | ChargerSafety | |
| PA2 | UART2_TX | OUT | USB/UART debug and config | AF7; CP2104 bridge |
| PA3 | UART2_RX | IN | USB/UART debug and config | AF7; CP2104 bridge |
| PA11 | CAN_RX | IN | CAN receive via ISO1050 | AF9; bxCAN |
| PA12 | CAN_TX | OUT | CAN transmit via ISO1050 | AF9; bxCAN |
| — | POWER_ENABLE | OUT | Holds board alive via power-latch circuit | GPIO TBD; must be de-asserted last on shutdown |
| — | POWER_BUTTON | IN | Wake/power-button input | GPIO TBD; edge detect |
| — | CHARGE_DETECT | IN | Charger presence detection input | GPIO TBD |
| PA13 | SWDIO | BIDIR | SWD debug/programming data | Pulled up; AF0 |
| PA14 | SWDCK | IN | SWD debug/programming clock | Pulled down; AF0 |
| — | NRST | IN | Reset pin | External reset; no internal pull needed if board provides |
| BOOT0 | IN | Boot mode selection | Low = normal boot; High = system bootloader; board must pull low for normal operation |

> **OPEN QUESTION:** Exact GPIO for POWER_ENABLE, POWER_BUTTON, CHARGE_DETECT — confirm from schematic.
> **RESOLVED:** Permission output polarity confirmed (schematic reviewed). See §11 for details.
> **OPEN QUESTION:** ISL28022 I2C address (A0/A1 pin strap) — confirm from board.
> **OPEN QUESTION:** BOOT0 pull resistor value and board pull direction.

---

## 2. Electrical Domains

| Domain | Description |
|---|---|
| LV / Digital (3.3V) | STM32F303, CP2104, LTC6820 master side, ISL28022 |
| HV Isolated (battery stack) | LTC6812 devices, isoSPI isolation boundary |
| CAN Isolated | ISO1050 provides galvanic isolation between CAN bus and STM32 |
| Vpack / Load-side | Load bus voltage; measured by PA1 ADC after resistor divider |
| Vbat / Battery-side | Battery terminal voltage; measured by ISL28022 |

The LTC6820 provides the isolation barrier between the 3.3V SPI domain and the HV isoSPI chain. All signals crossing the isoSPI boundary are current-mode differential signals per LTC6820 datasheet.

---

## 3. CELL isoSPI Chain Topology

```
STM32 SPI1 ──► LTC6820 (CS_CELL=PA4)
                    │
                 isoSPI
                    │
              [LTC6812 #0] ──► [LTC6812 #1] ──► ... ──► [LTC6812 #4]
              (Cells 1-15)   (Cells 16-30)          (Cells 61-75)
```

- 5 × LTC6812 in standard daisy-chain
- Broadcast (address-all) commands for ADCV, WRCFG, etc.
- Read-back in ascending physical device order (device 0 = lowest in stack)
- `isospi_reverse` orientation: **OPEN QUESTION** — verify from board layout whether MOSI enters at the top or bottom of the stack. Set `CELL_CHAIN_ISOSPI_REVERSE` accordingly.
- Total voltage channels: 75 (15 × 5)
- Cell numbering: global cell index = (device_index × 15) + local_cell_index, zero-based

---

## 4. TEMP isoSPI Chain Topology

```
STM32 SPI1 ──► LTC6820 (CS_TEMP=PB12)
                    │
                 isoSPI
                    │
              [LTC6812 #0] ──► [LTC6812 #1] ──► ... ──► [LTC6812 #4]
              (Sensors 0-14) (Sensors 15-29)       (Sensors 60-74)
```

- Separate 5 × LTC6812 chain, physically isolated from CELL chain
- **S outputs on TEMP chain are sensor-bias enables only** — they are NOT balancing outputs
- **C inputs on TEMP chain measure Enepaq sensor voltage** — they are NOT cell voltages
- No DCC/balancing bits may ever be written to TEMP chain devices
- `isospi_reverse` orientation: **OPEN QUESTION** — verify from board layout.

---

## 5. LTC6820 Master Assumptions

- LTC6820 configured as isoSPI master
- STM32 SPI1: Mode 3, CPOL=1, CPHA=1
- SPI clock rate: max 1 MHz recommended for initial bring-up; datasheet allows higher (OPEN QUESTION: validated max rate on this board)
- Both CS_CELL and CS_TEMP idle HIGH
- CS toggling: CS assert → SPI transfer → CS deassert; minimum CS-to-SCK and SCK-to-CS hold per LTC6820 datasheet (t_CSS ≥ 50 ns, t_CSH ≥ 50 ns)
- Only one CS may be asserted at a time; firmware must enforce this with a mutex or sequencing guarantee
- LTC6820 MISO line is shared; the inactive LTC6820 (CS deasserted) must present high impedance — LTC6820 three-states MISO when CS is not selected (datasheet-backed assumption; verify if board uses open-drain or active drive)

---

## 6. LTC6812 CELL-Chain Role

| Parameter | Value |
|---|---|
| Devices | 5 |
| Channels per device | 15 |
| Total channels | 75 |
| Input signals | Cell voltages (battery chemistry) |
| ADC resolution | 16-bit; LSB = 100 µV |
| Measurement time (all cells, 7kHz filter) | ~1.1 ms per device (245 µs at 27kHz) |
| Balancing outputs | DCC1–DCC15 per device (passive shunt via external resistors) |
| OV/UV hardware threshold | Configurable in CFGRA; firmware evaluates independently as well |
| CFGRA UV register formula | `VUVCMP = (VUV_mV × 10) / 16 - 1` (12-bit) |
| CFGRA OV register formula | `VOVCMP = (VOV_mV × 10) / 16` (12-bit) |
| Wake-up method | CS pulse ≥ 10 µs with SPI clocking (datasheet: t_WAKE minimum) |
| Sleep after inactivity | ~1.8 s from last SPI activity |
| PEC | CRC-15 using polynomial 0x4599 |

> **Datasheet-backed:** 100 µV/LSB cell voltage, PEC15 polynomial 0x4599, 15 cells per device, register layout (RDCVA–RDCVE = 3 cells each), CFGRA[2:0]=VUV[11:8], CFGRA[4:3]/CFGRA[5]=DCC.

---

## 7. LTC6812 TEMP-Chain Role

| Parameter | Value |
|---|---|
| Devices | 5 |
| Channels per device | 15 C-input measurements |
| Total channels | 75 |
| Input signals | Enepaq temperature sensor voltage measured through LTC6812 C-input pairs |
| S outputs | Sensor-bias enable controls only; enabled temporarily during measurement |
| CELL balancing on TEMP chain | Forbidden |
| DCC/S-output use on TEMP chain | Allowed only through a dedicated TEMP sensor-bias API |
| Conversion command | ADCV, because the sensors are measured on C-inputs |
| Read command | RDCVA / RDCVB / RDCVC / RDCVD / RDCVE |

The TEMP-chain LTC6812 devices are used as floating multi-channel voltage ADCs for Enepaq temperature sensor voltages. The measured channels are the LTC6812 C-input measurements, not GPIO/AUX measurements.

The TEMP-chain S outputs are reused only as temporary sensor-bias enables. They must never be exposed through the CELL-balancing API. Any generic balancing/DCC write to the TEMP chain is a firmware error.

---

## 8. Enepaq Temperature Sensor Measurement Model

The Enepaq/Sony-Murata temperature sensor behaves as a temperature-variable voltage shunt/reference. The TEMP-chain LTC6812 measures the resulting sensor voltage through its C-input measurement path.

Measurement sequence:

1. Select TEMP chain.
2. Write TEMP-chain configuration to enable the required S-output sensor-bias switches.
3. Wait `TEMP_SETTLE_TIME_MS`.
4. Issue `ADCV` on the TEMP chain.
5. Wait for conversion completion or poll as supported.
6. Read `RDCVA` through `RDCVE`.
7. Clear all TEMP-chain S-output sensor-bias enables.
8. Convert each measured voltage to temperature using the Enepaq voltage-temperature table.
9. Mark any out-of-range channel invalid.
10. Require all configured/required TEMP channels to be fresh and valid before declaring temperature coverage valid.

S outputs must be cleared on success and on every failure/error path.

> **RESOLVED:** V-T table populated from Sony-Murata NTC Table 5 — 33 breakpoints, −40°C to +120°C, 2440 mV to 1300 mV. Implemented in `firmware/src/bms/bms_measurements.c` as `k_enepaq_vt[]`.
> **OPEN QUESTION:** Maximum number of sensors that can be biased simultaneously (thermal/power budget of bias resistors). Current firmware biases all 75 simultaneously; verify power/thermal budget on hardware.

---

## 9. ISL28022 and Vpack Measurement Paths

| Signal | Source | Path | Notes |
|---|---|---|---|
| Vbat | Battery terminal | ISL28022 V_bus register | Battery-side voltage; 16-bit; 4 mV LSB (32V range) or 8 mV LSB (60V range) |
| I_batt | Shunt resistor | ISL28022 V_shunt register | Current via CSM2F-8518 shunt; programmable gain |
| Vpack | Load side / precharge bus | PA1 ADC (12-bit, 3.3V ref) | Scaled by external resistor divider; calibrated in config |

**ISL28022 configuration:**
- I2C address: 0x40 (A0 and A1 unconnected = pulled low = default address). **Verify on board before first I2C test** (HV-4).
- Shunt resistor: CSM2F-8518-L100J01, 0.1 mΩ (0.0001 Ω). Current path includes AMC1302 isolation amplifier; full current scaling requires calibration (see `current_gain_x1000` in config).
- Register 0x00 (Configuration): V_bus 60V range, PGA/8 (±320 mV shunt), 12-bit continuous mode.
- Register 0x01 (Shunt Voltage): signed 16-bit, 80 µV/LSB with PGA=/8 (firmware setting). Calibration register set to 0 — raw shunt voltage returned; scaling applied in `bms_measurements.c`.
- Register 0x02 (Bus Voltage): unsigned 16-bit, 4 mV/LSB (right-shift 3). Vbat = Vbus_reading × ~38.93 (front-end gain from OPA2197 conditioning, Voutmax ≈ 11.56 V at Vinmax = 450 V). Scaling constant stored in `vbat_gain_x1000` config field; requires board calibration.
- I2C rate: 100 kHz standard mode (conservative for bring-up; 4.75 kΩ external pull-ups).

**PA1 Vpack ADC:**
- STM32F303 ADC1 channel 2 (PA1)
- 12-bit; VREF+ = 3.3V (or dedicated Vref — OPEN QUESTION)
- External divider ratio: **OPEN QUESTION — confirm from schematic to establish calibration constants**
- Vpack = (ADC_raw / 4095) × VREF × VPACK_DIVIDER_RATIO + VPACK_OFFSET
- Calibration constants stored in config: `vpack_gain`, `vpack_offset`

---

## 10. Vbat vs Vpack Distinction

This is a critical distinction for precharge validation:

| Parameter | Vbat | Vpack |
|---|---|---|
| Physical node | Battery positive terminal | Load / precharge bus |
| Sensor | ISL28022 V_bus | PA1 ADC |
| State when contactors open | Battery pack voltage | Load-side capacitor / bus voltage |
| State when precharging | Static (pack) | Rising toward Vbat |
| Precharge complete condition | — | Vpack ≥ PRECHARGE_PCT% × Vbat |

Firmware must **never** use Vbat as Vpack or vice versa. Invalid Vbat or invalid Vpack must independently block precharge completion judgment.

---

## 11. Shutdown / Permission Output Semantics

These outputs feed into the downstream shutdown/interlock circuit, not directly into contactor coils.

| Signal | Pin | Meaning when ASSERTED | MCU pin level | Safe (de-asserted) state |
|---|---|---|---|---|
| MASTER_OK | PB11 | BMS is healthy and operating; system may proceed | HIGH | MCU LOW → downstream HIGH (inactive) |
| DISCHARGE_PERM | PB10 | Discharge operation is permitted by BMS | HIGH | MCU LOW → downstream HIGH (inactive) |
| CHARGE_PERM | PB0 | Charge operation is permitted by BMS | HIGH | MCU LOW → downstream HIGH (inactive) |
| CHARGER_SAFETY | PB2 | Charger may safely apply voltage | HIGH | MCU LOW → downstream HIGH (inactive) |

**Polarity confirmed (schematic reviewed):** All four outputs use an identical N-channel MOSFET stage.
MCU HIGH → MOSFET on → drain pulled LOW → downstream active-low signal asserted.
MCU LOW → MOSFET off → downstream pulled HIGH (via pull-up) → signal inactive (safe default).
Implemented in `board_outputs.c`; safe state enforced by `board_outputs_init_safe()` at every reset.

> **OPEN QUESTION:** Does MASTER_OK have watchdog / heartbeat implications on the downstream circuit?

**Rule:** After reset, all outputs must be in the safe (de-asserted) state. The `board_outputs` BSP layer owns the polarity mapping. The rest of firmware uses logical `ASSERTED` / `DEASSERTED` enums.

---

## 12. Power Latch Behaviour

```
Wake sources: POWER_BUTTON edge, CHARGE_DETECT rising
      │
      ▼
MCU starts via LM5165 / power-latch circuit
      │
      ▼
Firmware asserts POWER_ENABLE GPIO to hold latch alive
      │
      ▼
Normal operation
      │
Shutdown request (protocol, fault, button)
      ▼
1. bms_outputs: deassert all permissions
2. Complete any in-flight flash writes
3. bms_outputs: deassert POWER_ENABLE
```

POWER_ENABLE must **not** be released until all outputs are safe. Releasing POWER_ENABLE while a permission is asserted is a firmware error.

> **OPEN QUESTION:** LM5165 specific sequencing/hold time requirements for POWER_ENABLE.
> **OPEN QUESTION:** Is there a minimum on-time before power-down is permitted?

---

## 13. USB / CAN Role

| Interface | Path | Role |
|---|---|---|
| USB/UART | PA2/PA3 → CP2104 | Desktop tool protocol; debug logging; config; firmware update entry |
| CAN | PA11/PA12 → ISO1050 | Telemetry to external BMS master; may receive commands |

Neither interface is a safety authority. CAN or USB activity alone must not cause permission assertions. Loss of CAN/USB communication does not by itself trip a safety fault (configurable timeout may be used for CAN watchdog — policy OPEN QUESTION).

---

## 14. Datasheet-Backed Assumptions

| Assumption | Source |
|---|---|
| LTC6812 cell voltage LSB = 100 µV | LTC6812 datasheet §7.1 |
| LTC6812 PEC polynomial = 0x4599 (CRC-15) | LTC6812 datasheet §8 |
| LTC6812 15 cell channels per device | LTC6812 datasheet §1 |
| LTC6812 wakeup: CS pulse while SPI clocking, t_WAKE ≥ 300 µs | LTC6812 datasheet §6.3 |
| LTC6812 sleep after ~1.8s inactivity | LTC6812 datasheet §6.4 |
| LTC6812 ADCV all cells: 7kHz mode ~1.1ms, 27kHz mode ~245µs | LTC6812 datasheet Table 4 |
| LTC6812 CFGRA UV = (VUV × 10000)/16 - 1 (12-bit) | LTC6812 datasheet §9.1 |
| LTC6812 CFGRA OV = (VOV × 10000)/16 (12-bit) | LTC6812 datasheet §9.1 |
| LTC6820 SPI Mode 3, master | LTC6820 datasheet |
| ISL28022 V_shunt LSB = 10 µV | ISL28022 datasheet Table 3 |
| ISL28022 V_bus LSB = 4 mV (32V range) | ISL28022 datasheet Table 3 |
| STM32F303 SPI1 AF5 (PA5/PA6/PA7) | STM32F303 datasheet Table 14 |
| STM32F303 I2C2 AF4 (PA9/PA10) | STM32F303 datasheet Table 14 |
| STM32F303 USART2 AF7 (PA2/PA3) | STM32F303 datasheet Table 14 |
| STM32F303 CAN AF9 (PA11/PA12) | STM32F303 datasheet Table 14 |

---

## 15. Board-Contract Assumptions

These are assumed based on the hardware description and must be verified against schematic:

1. CS_CELL and CS_TEMP are never asserted simultaneously — guaranteed by firmware sequencing.
2. Permission output GPIOs have sufficient drive strength for downstream circuit input impedance.
3. VPACK resistor divider does not exceed PA1 ADC input clamp range under any operating condition.
4. ISL28022 ALERT pin (if present) is either connected or pulled appropriately.
5. LTC6820 MISO is only driven when its CS is asserted (three-state assumed per datasheet).
6. All LTC6812 VCC supply is stable before firmware begins isoSPI transactions.

---

## 16. Hardware Validation Questions

| # | Question | Impact if wrong |
|---|---|---|
| HV-1 | isospi_reverse orientation for CELL chain | Cell-to-device mapping wrong; wrong cells reported/balanced |
| HV-2 | isospi_reverse orientation for TEMP chain | Sensor-to-device mapping wrong |
| HV-3 | ~~Active polarity of all 4 permission GPIOs~~ **RESOLVED** — MCU HIGH = asserted via MOSFET; see §11 | — |
| HV-4 | ISL28022 I2C address — A0/A1 unconnected assumed LOW → 0x40. **Verify on board with DMM before first isl28022_init() call.** | I2C communication fails |
| HV-5 | CSM2F-8518-L100J01 shunt resistance confirmed 0.1 mΩ. AMC1302 gain and current scaling must be calibrated on bench. | Current reading wrong without calibration |
| HV-6 | Vpack resistor divider ratio | Pack voltage reading wrong; precharge validation wrong |
| HV-7 | POWER_ENABLE GPIO pin | Power latch fails |
| HV-8 | POWER_BUTTON and CHARGE_DETECT GPIO pins | Wake detection fails |
| HV-9 | LTC6812 S-output to sensor bias mapping (which S# → which sensor) | Sensor bias sequence wrong |
| HV-10 | Maximum TEMP sensor bias-on time before thermal concern | Settle time configuration constraint |
| HV-11 | STM32F303 exact variant (CC/VC/RD/RE) | Flash map sizing |
| HV-12 | BOOT0 pull direction on board | Normal vs system bootloader boot |
| HV-13 | VREF+ source for STM32 ADC (internal or external) | Vpack calibration reference accuracy |
