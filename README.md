# BMS Firmware

STM32F303VC-based Battery Management System — firmware, desktop tool, and complete
pre-hardware validation stack.

> **No hardware has been flashed yet.**  
> All workflows below run entirely in simulation unless noted otherwise.  
> See [First-Flash Warning](#first-flash-warning) before connecting real hardware.

---

## Quick Start

```bash
# 1. Clone and enter the repo
git clone <repo-url>
cd BMS_Firmware

# 2. Install dev environment (creates .venv, checks toolchain)
./scripts/setup_dev_env.sh

# 3. Run the full validation suite (no hardware needed)
./scripts/validate_all.sh

# 4. Run the interactive CLI demo against the fake target
./scripts/demo_local.sh

# 5. Launch the desktop GUI with the fake target
./scripts/run_gui.sh --fake --mode healthy
```

---

## Project Structure

```
BMS_Firmware/
├── firmware/               STM32F303VC application firmware (C11)
│   ├── src/
│   │   ├── bms/            Core BMS logic (config, faults, balance, measurements)
│   │   ├── drivers/        LTC6812 isoSPI driver, ISL28022 I2C power monitor
│   │   └── board/          STM32 BSP (SPI, UART, outputs, clock, flash)
│   ├── include/            Public headers
│   └── cmake/              Toolchain file + CMakeLists.txt
├── bootloader/             STM32 bootloader (sector 0, 32 KB)
│   ├── src/                bl_validate.c, bl_jump.c, bl_update.c
│   └── include/
├── tool/                   Python desktop tool (CLI + GUI)
│   ├── src/
│   │   ├── core/           AppState, TargetModel, PollingLoop, ConnectionManager
│   │   ├── protocol/       Framing, CRC, BmsProtocolClient, packet_defs
│   │   ├── config/         BmsConfig schema, validator
│   │   ├── update/         Package builder/parser, BootloaderUpdater, ST-Link wrapper
│   │   ├── fake_target/    Full BMS simulator — 12 modes, TCP or in-process
│   │   ├── cli/            bmsctl.py — thin CLI over the shared backend
│   │   ├── gui/            PyQt6 desktop app (9 tabs)
│   │   └── connection/     DeviceState, DeviceMode, CapabilitiesState
│   ├── tests/              272+ pytest tests (no hardware required)
│   └── requirements.txt    pyserial, PyYAML, pytest, PyQt6
├── tests/                  C unit tests (Unity framework)
│   ├── unit/               test_pec15.c, test_balance.c, test_openwire.c, …
│   ├── mock_bsp/           Mock SPI/UART/outputs/clock/flash
│   └── vendor/unity/
├── scripts/
│   ├── setup_dev_env.sh        One-command dev environment setup
│   ├── validate_all.sh         One-command full validation
│   ├── demo_local.sh           Full stack demo (fake target, CLI, optional GUI)
│   ├── run_gui.sh              Launch GUI (with optional fake target)
│   ├── bmsctl.sh               CLI wrapper (activates .venv)
│   ├── build_firmware.sh       STM32 firmware build
│   ├── flash_stlink.sh         Dry-run and real flash via ST-Link (--execute required)
│   ├── first_flash_dry_run.sh  Pre-hardware readiness check (no flash)
│   └── package_release.sh      Create dist/ release bundle
├── protocol/               Protocol specification documents
├── docs/                   Design documents (hardware contract, safety model, etc.)
└── build_firmware/         CMake build output (gitignored)
```

---

## Setup

```bash
./scripts/setup_dev_env.sh
```

Creates `.venv`, installs `tool/requirements.txt`, and checks for:

| Tool | Required for | Install |
|------|-------------|---------|
| Python 3.11+ | Everything | https://python.org |
| arm-none-eabi-gcc | Firmware build | brew install --cask gcc-arm-embedded |
| cmake | Firmware build | brew install cmake |
| ninja | Firmware build | brew install ninja |
| clang | C unit tests | included with Xcode CLT |
| STM32_Programmer_CLI | ST-Link flash (optional) | https://st.com/stm32cubeprog |
| PyQt6 | Desktop GUI | pip install PyQt6 |

---

## Validate Everything

```bash
./scripts/validate_all.sh                   # full suite with firmware build
./scripts/validate_all.sh --no-firmware     # skip STM32 build
```

Runs: tool detection → pytest suite → fake-target self-test → config round-trip →
package + update simulation → firmware build.

Expected output: `19+ passed  0 failed  0-2 skipped` (skipped = optional tools absent).

---

## Run Tests

### Python tests (272+)
```bash
python3 -m pytest tool/tests/ -q
```

### C unit tests (50+)
```bash
bash build_tests/run_tests.sh
```

Covers: PEC-15, measurements decode, protocol CRC, config validate/masks, outputs,
flash layout, bootloader validate, faults, cell balancing, open-wire detection.

---

## Build Firmware

```bash
export PATH="/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin:$PATH"
./scripts/build_firmware.sh           # release build
./scripts/build_firmware.sh debug     # debug build
```

Artifacts in `build_firmware/`:

| File | Description |
|------|-------------|
| `firmware.bin` | Raw binary, flash at 0x08008000 |
| `firmware.hex` | Intel HEX, flash at 0x08008000 |
| `bms_firmware.elf` | ELF with debug symbols |
| `firmware.map` | Linker map (section sizes, symbols) |

---

## CLI Tool

```bash
./scripts/bmsctl.sh --help
```

Activates `.venv` automatically.

```bash
# Start fake target in one terminal
./scripts/bmsctl.sh fake-target run --mode healthy

# In another terminal
./scripts/bmsctl.sh connect
./scripts/bmsctl.sh values
./scripts/bmsctl.sh cells -v
./scripts/bmsctl.sh temps
./scripts/bmsctl.sh faults --json
./scripts/bmsctl.sh diagnostics
./scripts/bmsctl.sh openwire run

# Config
./scripts/bmsctl.sh config export-default --out default.bin
./scripts/bmsctl.sh config validate default.bin
./scripts/bmsctl.sh config export-json default.bin
./scripts/bmsctl.sh config apply-ram default.bin

# Package + update
./scripts/bmsctl.sh package build firmware.bin fw.pkg --version 1.0.0
./scripts/bmsctl.sh package validate fw.pkg
./scripts/bmsctl.sh update simulate fw.pkg    # test protocol update flow
./scripts/bmsctl.sh update dry-run fw.pkg     # print steps without running
```

---

## Desktop GUI

```bash
./scripts/run_gui.sh --fake                          # healthy mode
./scripts/run_gui.sh --fake --mode openwire_detected # test open-wire tab
./scripts/run_gui.sh --fake --mode cell_uv           # test undervoltage fault
./scripts/run_gui.sh --fake --mode bootloader        # test update simulation
```

**Tabs:**
| Tab | Description |
|-----|-------------|
| Connection | TCP (fake target) or serial port; device capabilities |
| Bring-Up | Diagnostics counters, GPIO snapshot, chain probes, one-shot measurements, open-wire, balance disable-all |
| Dashboard | Pack voltage, current, state, uptime, polling controls |
| Cells | 75-cell voltage table; measure-once; invalid cells highlighted |
| Temperatures | 75-sensor table; measure-once; invalid sensors highlighted |
| Faults | Active + latched bitmaps; named fault list; clear latched |
| Config | Read/load/save/validate/apply-RAM; export default |
| Firmware Flash | Package inspect/validate; protocol update simulation; ST-Link gate |
| Logs | Event + packet logs |

---

## Fake Target

Simulates the full BMS protocol stack. Available modes:

| Mode | Description |
|------|-------------|
| `healthy` | 75 cells at 3700 mV, 25°C, no faults |
| `safe_invalid` | Zero cells, all temps INVALID, no faults |
| `cell_uv` | cell[0]=2400 mV, FAULT_CELL_UV active |
| `cell_ov` | cell[0]=4300 mV, FAULT_CELL_OV active |
| `temp_invalid` | All temps INVALID, FAULT_TEMP_READ_INVALID |
| `vpack_invalid` | FAULT_VPACK_INVALID active |
| `isospi_fault` | FAULT_ISOSPI_CELL active |
| `config_error` | FAULT_CONFIG_INVALID active |
| `precharge_fault` | FAULT_PRECHARGE_TIMEOUT active |
| `bootloader` | Responds as FIRMWARE_TYPE_BOOTLOADER |
| `openwire_detected` | Open-wire scan returns cell[0] flagged |
| `openwire_pec_fail` | Open-wire scan returns PEC error status |

Self-test (all modes):
```bash
./scripts/bmsctl.sh fake-target self-test
```

---

## Full Demo

```bash
./scripts/demo_local.sh             # CLI demo: connect, measure, config, package
./scripts/demo_local.sh --gui       # same + launch GUI at end
```

---

## First-Flash Preparation

> **No hardware has been flashed yet.** The firmware has been validated in simulation only.
> Read the documents listed below before connecting any hardware.

```bash
# 1. Validate the full software stack (no hardware required)
./scripts/validate_all.sh --no-firmware

# 2. Build firmware (requires arm-none-eabi-gcc)
export PATH="/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin:$PATH"
./scripts/build_firmware.sh

# 3. Run the first-flash readiness check (dry-run only, no real flash)
./scripts/first_flash_dry_run.sh

# 4. Read before connecting hardware (in order):
#    docs/bench_safety_checklist.md
#    docs/first_flash_guide.md
#    docs/uart_smoke_test.md
#    docs/01_hardware_contract.md  (especially §16 hardware validation questions)
```

Flash command (after safety checklist is complete):

```bash
# Dry-run first — prints exact command, does not flash
./scripts/flash_stlink.sh --app build_firmware/firmware.bin

# Real flash — requires --execute
./scripts/flash_stlink.sh --app build_firmware/firmware.bin --execute
```

First CLI session after flash:

```bash
PORT=/dev/tty.usbserial-XXXX   # find with: ls /dev/tty.usbserial-*
./scripts/bmsctl.sh connect    --serial $PORT
./scripts/bmsctl.sh diagnostics --serial $PORT
./scripts/bmsctl.sh diag gpio   --serial $PORT
./scripts/bmsctl.sh diag outputs --serial $PORT
```

### First-Flash Documents

| Document | Read when |
|----------|-----------|
| `docs/bench_safety_checklist.md` | Before applying power to the board |
| `docs/first_flash_guide.md` | Full first-flash procedure with pass/fail checklist |
| `docs/uart_smoke_test.md` | Diagnosing serial / protocol connection issues |
| `docs/01_hardware_contract.md` | Pin table, isoSPI topology, open hardware questions |
| `docs/02_safety_model.md` | Permission output semantics and safety invariants |

---

## Package Release

```bash
./scripts/package_release.sh
```

Creates `dist/bms-v{VERSION}/` containing firmware artifacts, Python tool source,
scripts (including `flash_stlink.sh` and `first_flash_dry_run.sh`), docs (including all
first-flash guides), and auto-generated release notes and manifest. No `.venv`,
`__pycache__`, or build caches are included.

---

## What Is Simulated (no hardware needed)

- All 12 fake target modes including fault injection, openwire, bootloader
- Full BMS protocol (framing, CRC, all packet types)
- Firmware update flow: enter-bootloader → begin → chunks → finalize
- Config read/write/validate/apply-RAM
- Cell balancing logic (C unit tests with mock SPI)
- Open-wire detection algorithm (C unit tests with mock SPI)
- Fault bit mapping and masking (C unit tests)
- Package build/parse/validate (Python tests)
- GUI polling, state display, all page interactions
- ST-Link dry-run (generates command without executing)

---

## What Remains Hardware-Dependent

| Item | Status |
|------|--------|
| UART/serial communication with real target | Not tested |
| isoSPI chain bring-up (LTC6812) | Not tested |
| ISL28022 I2C power monitor | Not tested |
| ADC calibration (Vpack, Vbat, Ibat) | Not tested |
| GPIO output states (MasterOK, Discharge, Charge, ChargerSafety) | Not tested |
| Precharge timing | Not tested |
| CAN bus | Not tested |
| Power-latch / power-button | Not tested |
| Thermal runaway under real cell conditions | Not tested |
| Actual flash/erase timing on STM32 | Not tested |
| Bootloader PEC verification on real SPI | Not tested |

---

## First-Flash Warning

> **Read this before connecting any hardware.**

1. The firmware has **not been tested on hardware**. Flash into a bench setup
   with no high-voltage pack attached.
2. `board_outputs_init_safe()` deasserts all permission outputs on startup.
   Verify this physically before attaching any contactor or interlock circuit.
3. Review `docs/01_hardware_contract.md` (pin table) and
   `docs/02_safety_model.md` (safety boundary) before first power-on.
4. Do not enable cell balancing until voltage measurements have been verified
   against a known-good reference.
5. Do not enable charging/discharging permission outputs until all fault
   thresholds have been reviewed and the config has been validated.
6. Use `./scripts/bmsctl.sh diagnostics` on first connect to verify PEC/I2C
   error counters are zero before trusting any measurement.
7. Run `./scripts/bmsctl.sh openwire run` before enabling balance to confirm
   all cell connections are intact.

---

## Design Documents

| Document | Content |
|----------|---------|
| `docs/00_system_purpose.md` | Purpose, safety boundary, what BMS does/doesn't do |
| `docs/01_hardware_contract.md` | Pin table, signal directions, notes |
| `docs/02_safety_model.md` | Fault taxonomy, permission outputs, invariants |
| `docs/03_firmware_architecture.md` | Module structure, state machine, polling loop |
| `docs/04_protocol_contract.md` | Packet format, all packet IDs, error codes |
| `docs/05_config_schema.md` | All config fields, types, ranges, defaults |
| `docs/06_flash_and_bootloader.md` | Flash layout, bootloader update flow |
| `docs/07_desktop_tool_design.md` | Tool architecture, GUI tabs, fake target strategy |
| `docs/08_validation_plan.md` | Test strategy, hardware bring-up checklist |
