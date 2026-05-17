# First-Flash Guide — STM32F303VC BMS Firmware

> **WARNING: Pre-hardware validation only.**
> This guide is for a controlled bench session with no high-voltage accumulator, no contactor
> actuation path, no vehicle operation, and no load/charger connected. This is not a safety
> sign-off. The firmware has not been tested on hardware. Follow
> [docs/bench_safety_checklist.md](bench_safety_checklist.md) before applying power.

---

## 1. Scope and Constraints

| Constraint | Requirement |
|---|---|
| Accumulator | Not connected — bench supply only, 3.3V LV domain |
| High voltage | Not present |
| Contactor / interlock path | Not connected if at all avoidable |
| Load / inverter | Not connected |
| Charger | Not connected |
| Vehicle operation | Not applicable |
| Safety sign-off | This session is not a safety sign-off |

Hardware validation questions (from `docs/01_hardware_contract.md`) that remain open:
- HV-3: Active polarity of permission outputs — **confirmed: MCU HIGH = active via MOSFET stage** (see §11 of 01_hardware_contract.md)
- HV-1/HV-2: `isospi_reverse` orientation for CELL and TEMP chains
- HV-4: ISL28022 I2C address (A0/A1 pin strap)
- HV-6: Vpack resistor divider ratio
- HV-7/HV-8: POWER_ENABLE / POWER_BUTTON / CHARGE_DETECT GPIO assignments

Do not enable any permission outputs or run cell balancing until the open questions for
that subsystem have been answered from schematic review.

---

## 2. Required Tools

| Tool | Purpose | Install |
|---|---|---|
| ST-Link V2 / V3 | SWD debug and programming interface | Hardware device |
| STM32_Programmer_CLI | Command-line flash tool | [st.com/stm32cubeprog](https://www.st.com/en/development-tools/stm32cubeprog.html) |
| USB/UART adapter (CP2104 on board, or external) | Serial protocol connection | Included on board via PA2/PA3 |
| DMM | Verify supply rails, output pin levels | Lab equipment |
| Oscilloscope or logic analyser (optional) | SPI / UART debug | Recommended for isoSPI bring-up |
| Bench supply (3.3V, current-limited to ~200 mA) | LV domain power | Lab equipment |
| Python 3.11+ and `.venv` | Desktop tool / bmsctl | `./scripts/setup_dev_env.sh` |

---

## 3. Build Commands

Run on the development host before the bench session.

```bash
# Set ARM toolchain on PATH (adjust path to your toolchain install)
export PATH="/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin:$PATH"

# Full validation suite (no hardware required)
./scripts/validate_all.sh

# Firmware build
./scripts/build_firmware.sh
```

Both commands must exit 0 before proceeding.

Expected `validate_all.sh` output: `33 passed  0 failed  0-5 skipped`

Expected `build_firmware.sh` output: `build_firmware/firmware.bin` created; size printed.

---

## 4. Firmware Artifacts

All artifacts are in `build_firmware/` after a successful build.

| File | Description | Use |
|---|---|---|
| `build_firmware/firmware.bin` | Raw binary image | Flash via ST-Link at `APP_START_ADDR` |
| `build_firmware/firmware.hex` | Intel HEX (address embedded) | Alternative flash input |
| `build_firmware/bms_firmware.elf` | ELF with debug symbols | GDB debugging |
| `build_firmware/firmware.map` | Linker map (section sizes, symbol addresses) | Post-flash size verification |

To package for transport or distribution:

```bash
./scripts/package_release.sh --version 0.1.0
# Creates dist/bms-v0.1.0/ with all artifacts + docs
```

---

## 5. Flash Target and Memory Map

### MCU: STM32F303VC (Cortex-M4, 256 KB flash)

| Region | Start Address | Size | Notes |
|---|---|---|---|
| Bootloader | `0x08000000` | 32 KB | Do not overwrite on first flash |
| **Application** | **`0x08008000`** | **188 KB** | **Flash firmware.bin here** |
| Config slot A | `0x08037000` | 8 KB | Written by firmware; do not pre-erase |
| Config slot B | `0x08039000` | 8 KB | Written by firmware; do not pre-erase |

### What to flash on first session

**Flash `firmware.bin` at address `0x08008000` only.**

Do not flash to `0x08000000` (bootloader region) unless explicitly intending to update the
bootloader. A BMS bootloader now exists as a separate build artifact (`build_bootloader/bootloader.bin`),
but it has **NOT been hardware-validated** — only unit-tested in host simulation.
The STM32 factory system bootloader is sufficient for initial programming via SWD.
See `bootloader/README.md` for the bootloader build and first-flash procedure.

### Flash command (dry-run first)

```bash
# Dry-run — prints command, does not flash
./scripts/flash_stlink.sh --app build_firmware/firmware.bin

# Actual flash — requires --execute
./scripts/flash_stlink.sh --app build_firmware/firmware.bin --execute
```

Or using the package-based path (recommended — validates CRC before flashing):

```bash
# Build package
./scripts/bmsctl.sh package build build_firmware/firmware.bin fw.pkg --version 0.1.0

# Dry-run via bmsctl
./scripts/bmsctl.sh stlink dry-run-app fw.pkg
```

The exact `STM32_Programmer_CLI` command that will be run:

```
STM32_Programmer_CLI -c port=SWD freq=4000 reset=HWrst \
    -d build_firmware/firmware.bin -s 0x08008000 -v
```

- `-c port=SWD freq=4000 reset=HWrst` — SWD at 4 MHz, hardware reset
- `-d <file>` — file to download
- `-s 0x08008000` — start address (application region)
- `-v` — verify after write

---

## 6. Expected First Boot

After power-on or reset following a successful flash:

| Observable | Expected value | Notes |
|---|---|---|
| UART baud rate | **115200 baud, 8N1, no flow control** | PA2=TX, PA3=RX via CP2104 USB bridge |
| Protocol SOF bytes | `0xAA 0x55` | Every BMS frame starts with these two bytes |
| Firmware type | `FIRMWARE_TYPE_BMS_APP (0x0001)` | Reported in capabilities response |
| HW profile ID | `0x0001` | From `HW_PROFILE_ID` in `bms_constants.h` |
| Protocol version | `1` | From `PROTOCOL_VERSION` |
| Config schema version | `1` | From `CONFIG_SCHEMA_VERSION` |
| Cell count | `75` | 5 × LTC6812 × 15 cells |
| Feature flags | `0x00000007` | Cell voltage + Temperature + Balancing |
| Permission outputs | All inactive (MCU LOW) | `board_outputs_init_safe()` called at reset |
| CS_CELL (PA4) | HIGH (idle) | Active-low; asserted only during SPI transfer |
| CS_TEMP (PB12) | HIGH (idle) | Active-low; asserted only during SPI transfer |
| Default measurements | All INVALID | Expected — no LTC6812/ISL28022 connected yet |
| Default faults | TEMP_READ_INVALID, CELL_READ_INVALID, etc. | Expected with no sensors connected |
| IWDG | Active; 500 ms timeout | Firmware must service within 500 ms or reset |
| Main loop period | 10 ms | Polling loop tick rate |

**On first boot without sensors**, expect the following active faults:
- `FAULT_TEMP_READ_INVALID` — temperature measurement failed
- `FAULT_CELL_READ_INVALID` — cell voltage measurement failed
- `FAULT_VPACK_INVALID` — Vpack ADC read failed or out of range
- `FAULT_I2C_ISL28022` — ISL28022 I2C probe failed

These faults are expected on a bench without the full hardware chain. They confirm the
firmware boots and the measurement subsystem is attempting reads.

---

## 7. First Commands (bmsctl)

Open a terminal with the `.venv` active. Connect via serial port (CP2104):

```bash
# Find the serial port (macOS)
ls /dev/tty.usbserial-* /dev/tty.SLAB_USBtoUART* 2>/dev/null

# All commands below use: --serial /dev/tty.usbserial-XXXX
# Replace XXXX with your port. Default baud is 115200.
SPORT="--serial /dev/tty.usbserial-XXXX"
```

### Step 1 — Capabilities handshake

```bash
./scripts/bmsctl.sh connect $SPORT
```

Expected output fields: `mode`, `firmware_version`, `hw_profile_id`, `protocol_version`,
`config_schema_version`, `cell_count`, `feature_flags`.

Pass: command exits 0 and prints `mode: BMS_APP`.
Fail: connection refused, timeout, or mode shows `DISCONNECTED`.

### Step 2 — Diagnostics summary

```bash
./scripts/bmsctl.sh diagnostics $SPORT
```

Expected fields: `reset_cause`, `pec_cell_errors`, `pec_temp_errors`, `i2c_errors`,
`open_wire_valid`, `open_wire_mask`, `uptime_ms`.

Pass: `uptime_ms` > 0. `pec_cell_errors` and `pec_temp_errors` may be non-zero without
physical sensors — that is expected. `i2c_errors` > 0 is expected without ISL28022.

### Step 3 — GPIO snapshot

```bash
./scripts/bmsctl.sh diag gpio $SPORT
```

Expected: `cs_cell=1`, `cs_temp=1` (idle high).

Pass: both CS pins report HIGH at idle.
Fail: CS pins report LOW — firmware may be mid-transfer or GPIO init failed.

### Step 4 — Permission outputs

```bash
./scripts/bmsctl.sh diag outputs $SPORT
```

Expected: `logical_state=0x00` (all permissions inactive). `raw_state` reflects MCU GPIO
pin levels; MCU LOW = inactive for all four permission outputs (confirmed polarity).

Pass: `logical_state: 0x00  (master_ok=0 discharge=0 charge=0 charger_safety=0)`.
Fail: any permission bit is 1 — do not proceed; investigate firmware init path.

### Step 5 — isoSPI probe (CELL chain)

```bash
./scripts/bmsctl.sh probe cell-chain $SPORT
```

Expected with no hardware: all ICs report `NO_RESPONSE`. Status will be non-zero.

If LTC6812/LTC6820 chain is connected: expect `ic_count=5` with all 5 ICs responding.

### Step 6 — isoSPI probe (TEMP chain)

```bash
./scripts/bmsctl.sh probe temp-chain $SPORT
```

Same expectation as cell-chain probe.

### Step 7 — ISL28022 probe

```bash
./scripts/bmsctl.sh probe isl $SPORT
```

Expected with no ISL28022: `status: FAIL (I2C NACK)`.
Expected with ISL28022 connected and I2C address confirmed: `status: OK  config_reg: 0x....`.

### Step 8 — Vpack raw ADC read

```bash
./scripts/bmsctl.sh read vpack-raw $SPORT
```

Expected with no external voltage on PA1: raw ADC code near 0 or noise floor.
Expected with bench supply: raw code proportional to input voltage via divider.

### Step 9 — One-shot cell measurement

Only run after CELL chain probe passes (all 5 ICs respond).

```bash
./scripts/bmsctl.sh measure cells $SPORT
```

### Step 10 — One-shot temperature measurement

Only run after TEMP chain probe passes.

```bash
./scripts/bmsctl.sh measure temps $SPORT
```

### Step 11 — Power measurement

Only run after ISL28022 probe passes.

```bash
./scripts/bmsctl.sh measure power $SPORT
```

---

## 8. Pass/Fail Checklist

| # | Step | Command / Measurement | Expected Result | Fail Condition | Next Action |
|---|---|---|---|---|---|
| 1 | Flash | `flash_stlink.sh --app firmware.bin --execute` | Exit 0; "Download verified successfully" | Non-zero exit; verify error | Check ST-Link connection; see §9 ST-Link recovery |
| 2 | Boot | Power cycle; observe UART | Bytes appear within 500 ms | No UART output | See §9 UART recovery |
| 3 | Capabilities | `bmsctl connect --serial PORT` | `mode: BMS_APP` | `DISCONNECTED` or timeout | See §9 Protocol recovery |
| 4 | Uptime | `bmsctl diagnostics --serial PORT` | `uptime_ms > 0` | 0 or command fails | Check UART connection |
| 5 | CS idle | `bmsctl diag gpio --serial PORT` | `cs_cell=1, cs_temp=1` | Either CS = 0 at idle | Firmware SPI init issue |
| 6 | Outputs | `bmsctl diag outputs --serial PORT` | `logical_state: 0x00` | Any bit non-zero | STOP — investigate output polarity and init |
| 7 | CELL probe | `bmsctl probe cell-chain --serial PORT` | 5 ICs respond (if chain connected) | 0 ICs or PEC errors | See §9 CELL probe recovery |
| 8 | TEMP probe | `bmsctl probe temp-chain --serial PORT` | 5 ICs respond (if chain connected) | 0 ICs or PEC errors | See §9 TEMP probe recovery |
| 9 | ISL probe | `bmsctl probe isl --serial PORT` | `status: OK` (if ISL connected) | NACK | See §9 ISL recovery |
| 10 | Vpack raw | `bmsctl read vpack-raw --serial PORT` | `status: OK`; code near 0 without input | status FAIL | Check PA1 wiring |
| 11 | Cell measure | `bmsctl measure cells --serial PORT` | 75 cells, all valid | Any INVALID | Probe chain first |
| 12 | Temp measure | `bmsctl measure temps --serial PORT` | 75 temps, all valid | Any INVALID | Probe chain first |
| 13 | Power measure | `bmsctl measure power --serial PORT` | Vbat/Vpack/I valid | FAIL | ISL probe first |
| 14 | Faults | `bmsctl faults --serial PORT` | Only measurement faults from §6 | Permission/WATCHDOG fault | Investigate immediately |

---

## 9. Recovery Procedures

### ST-Link cannot connect

1. Verify ST-Link is recognised by the host: `STM32_Programmer_CLI -l` should list the probe.
2. Check SWD wiring: SWDIO → PA13, SWDCK → PA14, GND shared, VCC 3.3V reference.
3. Check BOOT0 is pulled LOW (normal boot mode). HIGH = system bootloader.
4. Check NRST is not held low by an external circuit.
5. Try lower SWD frequency: add `freq=1000` to the programmer command.
6. If the MCU is in a hard-fault loop, SWD may still work — try connect-under-reset:
   `-c port=SWD freq=1000 reset=HWrst`
7. If `FAULT_WATCHDOG` is in latched faults after successful connect, the MCU reset due to
   IWDG — this is normal if the boot was clean but USB was slow.

### Flash succeeds but no UART output

1. Verify the CP2104 USB bridge is powered and enumerated (check `/dev/tty.usbserial-*`).
2. Verify PA2 (TX) and PA3 (RX) wiring to CP2104.
3. Verify baud rate: 115200. Ensure no parity, 1 stop bit.
4. Check `board_uart_init()` is called early in `main()` — it is called before the main
   loop in the current firmware.
5. If UART appears but output is garbled: clock configuration issue — `board_clock_init()`
   may not have run correctly. Verify STM32 PLL settings in `board_clock.c`.
6. Put the MCU in SWD halt mode and single-step past `board_uart_init()` to confirm the
   USART registers are set correctly.

### UART works but no protocol response

1. The BMS framing layer expects `0xAA 0x55` start-of-frame (SOF) bytes. Verify the host
   is sending framed packets, not raw bytes.
2. Use `bmsctl connect --serial PORT` — it sends a capabilities request and waits for the
   framed response.
3. If no response after 2 seconds: the firmware RX interrupt or polling loop may not be
   running. Check `bms_protocol_poll()` is called in the main loop.
4. Try `bmsctl connect --serial PORT --json` for more detailed error output.
5. The fake target (TCP, no hardware) can be used to verify the tool stack independently:
   `./scripts/bmsctl.sh fake-target run` in one terminal, then
   `./scripts/bmsctl.sh connect` (TCP) in another.

### Permission outputs not inactive

1. STOP. Do not proceed until this is resolved.
2. `diag outputs` reports `logical_state != 0x00` — the firmware believes a permission is
   active. This should not happen at first boot.
3. Possible cause: `board_outputs_init_safe()` was not called or returned early due to a
   prior fault path. Check firmware startup sequence in `bms_main_loop.c`.
4. Possible cause: output polarity constant mismatch — confirmed polarity is MCU HIGH =
   active, MCU LOW = inactive for all four permission outputs. If logical_state is wrong,
   the firmware init path is suspect. Cross-check PB10/PB11/PB0/PB2 with a DMM.
5. Measure PB10, PB11, PB0, PB2 with a DMM. Record actual voltage levels and compare to
   what `diag outputs raw_state` reports. If they disagree, the GPIO readback is wrong.

### CS_CELL or CS_TEMP not idle HIGH

1. At idle (no SPI transfer in progress), PA4 (CS_CELL) and PB12 (CS_TEMP) must both be
   HIGH (3.3V measured at MCU pin).
2. If LOW: SPI or GPIO init may have left the CS asserted. Check `board_spi_init()` in
   `board_spi.c` — CS pins must be set HIGH immediately after GPIO init.
3. If oscillating: a measurement cycle may be running. This is unexpected immediately after
   boot before capabilities handshake.

### CELL chain probe fails (0 ICs respond)

1. Check physical isoSPI wiring from LTC6820 to first LTC6812 device.
2. Verify LTC6820 CS_CELL (PA4) is correctly wired and toggles during the probe (scope PA4).
3. Verify LTC6820 SCLK, MOSI, MISO lines (PA5, PA6, PA7) are connected.
4. LTC6812 may be asleep: the probe command includes a wake-up sequence; if it still fails,
   check LTC6812 VCC supply and the isoSPI differential pair continuity.
5. Check `CELL_CHAIN_ISOSPI_REVERSE` setting in config — if chain orientation is wrong,
   all devices may appear to not respond even if they are physically present.
6. PEC errors without responded=false usually indicate signal integrity issues (probe with
   oscilloscope at slow SPI clock first).

### TEMP chain probe fails

Same procedure as CELL chain, replacing PA4/CS_CELL with PB12/CS_TEMP.

### ISL28022 probe fails (I2C NACK)

1. Verify ISL28022 is powered (VCC supply).
2. Verify I2C wiring: PA9=SCL, PA10=SDA with 4.7 kΩ pull-ups to 3.3V.
3. Verify I2C address: default config assumes `0x40`. If A0 or A1 pin straps are HIGH,
   the address will be different. Use an I2C scanner or scope the SDA/SCL lines to
   observe the address being probed.
4. Update `ISL28022_I2C_ADDR` in `board_pins.h` if the address is wrong.

### Vpack reads invalid

1. PA1 ADC is `ADC1_IN2`. Verify the pin is not shorted or left floating.
2. Without an external voltage on the divider input, the raw code will be near 0 — this
   is OK. `status: OK` with `raw_code: 0` is a pass.
3. `status: FAIL` indicates the ADC conversion itself failed — check `board_adc_init()`.
4. After connecting a bench voltage to the divider input, verify the raw code scales
   proportionally. Record the divider ratio from the schematic (HV-6 open question).

### Watchdog / reset loop

1. The IWDG timeout is 500 ms. If the main loop is blocked longer than 500 ms, the MCU
   resets. `FAULT_WATCHDOG` will appear in latched faults after reconnect.
2. A reset loop (repeated IWDG resets) means the main loop is blocking indefinitely.
3. Use SWD to halt the MCU mid-loop and inspect the stack trace to find the blocking call.
4. Common cause: an SPI transaction waiting for a response from an unpopulated LTC6812 with
   no timeout. Check `LTC6812_MAX_RETRIES` and the per-operation timeout in `isospi.c`.

---

*See also: [bench_safety_checklist.md](bench_safety_checklist.md),
[uart_smoke_test.md](uart_smoke_test.md),
[01_hardware_contract.md](01_hardware_contract.md),
[02_safety_model.md](02_safety_model.md)*
