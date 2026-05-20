# Firmware

BMS firmware for STM32F303. C11, bare-metal, no RTOS.

## Folder Structure

```
firmware/
  src/
    bsp/           Board support layer (hardware init, GPIO, clocks, peripherals)
    drivers/       Device drivers (LTC6812, isoSPI, ISL28022, Vpack ADC)
    bms/           BMS application logic (measurements, faults, outputs, state, balance, config, protocol)
    main.c         Startup: init sequence, main loop
    bms_main_loop.c/h  Main loop orchestration
  include/
    bms_types.h    Shared enums, result codes, measurement structs
    bms_constants.h  Single source of truth for cross-layer constants
    bms_error.h    Error code definitions
  tests/           Host-compiled unit tests (mock BSP)
  linker/
    stm32f303_bms.ld   Linker script (provisional addresses; verify against variant)
  CMakeLists.txt
```

---

## Build System

CMake with arm-none-eabi-gcc. Use the provided scripts — they configure toolchain path and output directory automatically:

```bash
# Firmware (artifacts in build_firmware/):
./scripts/build_firmware.sh           # release
./scripts/build_firmware.sh debug     # debug

# Unit tests (native host, artifacts in build_tests/):
bash build_tests/run_tests.sh
```

Direct CMake invocations also work if you prefer:
```bash
cmake -B build_firmware -DCMAKE_TOOLCHAIN_FILE=cmake/arm_none_eabi.cmake
cmake --build build_firmware --target bms_firmware.elf
```

Firmware artifacts in `build_firmware/`:
- `bms_firmware.elf` — ELF with debug symbols
- `firmware.bin` — raw binary (flash at 0x08008000)
- `firmware.hex` — Intel HEX

Compiler flags: `-Wall -Wextra -Werror -std=c11 -Os` (release), `-Og -g3` (debug).

---

## Module Boundaries

Each module in `src/bms/` owns exactly the state and logic listed in `docs/03_firmware_architecture.md`. Hard rules:

1. **Only `bms_outputs` calls `board_output_set()`** — enforced by not including `board_outputs.h` in any other BMS module. Other modules call `bms_outputs_apply()` with a permission request struct.

2. **Only `bms_balance` calls `ltc6812_set_balance_mask()`** — enforced by include discipline.

3. **`bms_protocol` serializes/deserializes only** — it does not compute thresholds, evaluate faults, or write hardware.

4. **`ltc6812` driver refuses DCC writes to CHAIN_TEMP** — asserts at runtime, sets FATAL fault.

5. **Config is read-only to all modules except `bms_config`** — other modules receive a `const BmsConfig *` pointer from `bms_config_get()`.

---

## Safety Rules

These rules must be preserved in all code changes:

- All permission GPIOs must be in safe (deasserted) state before `board_outputs_init()` returns.
- `bms_measurements_run_temp_cycle()` must call `ltc6812_clear_s_outputs(CHAIN_TEMP)` in both success and error paths.
- The IWDG must be kicked inside every iteration of the main loop.
- Any `FAULT_*` at FATAL severity must call `bms_outputs_deassert_all()` and either halt or trigger IWDG.
- `bms_config_store()` validates the full config blob before any flash erase.
- No module uses last-known-good measurement values after `STALE_DATA_TIMEOUT_MS` for safety decisions.

---

## Config Rules

- Config is loaded once at boot via `bms_config_load()`.
- Modules receive a pointer via `bms_config_get()` — this pointer is valid for the lifetime of the application after successful load.
- If `bms_config_store()` writes a new config, the firmware performs a soft reset to re-initialize from the new config. There is no hot-swap of config during operation.
- The config struct layout must match `protocol/config_schema.yaml` exactly. If you add a field, update the YAML first, then implement.

---

## Coding Style

- C11; `snake_case` for all identifiers
- `typedef enum` with explicit underlying type where possible
- `typedef struct` with explicit packing annotation for any on-wire struct
- No dynamic memory allocation (`malloc`/`free`) anywhere in firmware
- No global mutable state except within the module that owns it (exported via getter functions)
- Magic numbers: never inline; always use a named constant in `bms_constants.h`
- Error returns: all functions that can fail return a `*Result` enum; callers must check
- No `printf` in production firmware; use the diagnostic ring buffer for logging

---

## Protocol Generation

The protocol packet definitions in `protocol/packet_ids.yaml` and config schema in `protocol/config_schema.yaml` are the authoritative source. A code-generation step (`scripts/generate_protocol.py`) produces:
- `include/bms_protocol_ids.h` — packet ID constants
- `include/bms_config_offsets.h` — config field offset/size constants (validated against C struct)

If you change a packet or config field, update the YAML, run the generator, and recompile.

---

## Hardware Abstraction

The BSP layer (`src/bsp/`) is the only layer that touches registers or hardware addresses. All drivers and BMS modules call BSP functions. This makes the `src/bms/` and `src/drivers/` layers portable to a simulator/test environment by substituting mock BSP implementations.

Mock BSP files live in `tests/mock_bsp/` and implement the same `board_*.h` interfaces as the real BSP.

---

## Unit Test Approach

Tests in `tests/unit/` compile only the module under test + mock BSP + required utilities. No hardware required.

Test framework: Unity (single-file C test framework, included in `tests/vendor/`). Run with CTest.

For each module, the test file:
1. Sets up mock BSP state
2. Calls the module function
3. Asserts on output state and BSP call trace

See `docs/08_validation_plan.md` for the full test list.
