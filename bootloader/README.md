# Bootloader

> **Status: Software-complete. NOT hardware-validated.**  
> Compiled, unit-tested in host simulation (23 passing tests), but never flashed to real hardware.  
> See [First-Flash Preparation](#first-flash-preparation) before connecting any hardware.

Separate STM32F303 firmware image occupying flash region `[0x08000000, 0x08007FFF]` (32 KB).

---

## Folder Structure

```
bootloader/
  src/
    main.c            Boot decision + UART init + protocol loop
    bl_uart.c/h       USART2 driver (PA2/PA3 AF7, 115200 baud @ HSI 8 MHz)
    bl_protocol.c/h   GET_CAPABILITIES, GET_BOOT_INFO, BOOT_UPDATE_* packets
    bl_flash.c/h      Flash erase/write/verify (STM32F303, direct registers, no HAL)
    bl_validate.c/h   Package header validation + CRC-32
    bl_jump.c/h       Safe jump to application (disable IRQs, set VTOR, MSP, branch)
  include/
    bl_config.h       Bootloader version, flash map constants
  linker/
    stm32f303vc_bootloader.ld  FLASH ORIGIN=0x08000000 LENGTH=32K with ASSERT guard
  CMakeLists.txt      Standalone build; reuses vendor/ from firmware/
```

---

## Build

```bash
export PATH="/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin:$PATH"
./scripts/build_bootloader.sh          # release build
./scripts/build_bootloader.sh debug    # debug build
```

Output in `build_bootloader/`:

| File | Description |
|------|-------------|
| `bootloader.bin` | Raw binary, flash at 0x08000000 |
| `bootloader.hex` | Intel HEX |
| `bms_bootloader.elf` | ELF with debug symbols |
| `bootloader.map` | Linker map |

---

## Boot Decision

```
1. RTC BKP0R == BL_ENTRY_FLAG → clear flag, stay in bootloader
2. App SP or reset vector invalid → stay in bootloader
3. Package header CRC or field validation fails → stay in bootloader
4. App image CRC-32 mismatch → stay in bootloader
5. All checks pass → jump to application at APP_START_ADDR
```

Entering bootloader mode from the application: set `RTC->BKP0R = 0xB007B007` then reset.

---

## Protocol

Packets supported (same BMS framing: SOF 0xAA 0x55, CRC-16/CCITT-FALSE big-endian):

| Packet | ID | Description |
|--------|----|-------------|
| `GET_CAPABILITIES` | 0x0001 | Returns `firmware_type=0x0002`, version, hw_profile, feature flags |
| `GET_BOOT_INFO`    | 0x0401 | Returns bootloader version and BL_ENTRY_FLAG magic |
| `BOOT_UPDATE_BEGIN` | 0x0403 | Validates 64-byte package header; sets up update context |
| `BOOT_UPDATE_CHUNK` | 0x0404 | Writes 256-byte chunk; erases pages lazily on first access |
| `BOOT_UPDATE_FINALIZE` | 0x0405 | Verifies CRC-32 over written image; returns computed CRC |
| `BOOT_UPDATE_ABORT` | 0x0406 | Resets update state machine |

Update flow: `BEGIN → CHUNK×N → FINALIZE` (or `ABORT` at any point).

Flash write strategy: pages (2 KB) are erased on the first halfword write to each page boundary — no separate pre-erase pass. Power loss mid-update leaves the app image invalid; the bootloader stays resident on the next reset.

---

## Flash Safety

Every erase and write call passes through `bl_flash_addr_in_app_region()`:
- Never below `APP_START_ADDR` (0x08008000)
- Never at or above `CONFIG_A_START_ADDR` (0x08037000)

Linker script ASSERT prevents the bootloader binary from exceeding 32 KB and overlapping the application region.

Compile-time assertions in `bl_config.h` and `bl_flash.c` verify the flash map at build time.

---

## Tests

Unit tests compiled for host (`BMS_HOST_BUILD=1`), no hardware required:

```bash
bash build_tests/run_tests.sh
```

`test_bootloader_protocol` (23 assertions):
- `GET_CAPABILITIES` fields and CRC
- `GET_BOOT_INFO` response
- Bad SOF / bad CRC / unknown packet rejected
- `BEGIN` with valid and invalid headers
- `CHUNK` in-order and out-of-order handling
- Full update flow with CRC verification
- `ABORT` state reset

---

## First-Flash Preparation

> The bootloader has never been flashed to hardware. Do not flash without completing the readiness checks.

```bash
# 1. Validate software stack (no hardware)
./scripts/validate_all.sh --no-firmware

# 2. Build bootloader and firmware
./scripts/build_bootloader.sh
./scripts/build_firmware.sh

# 3. Read before connecting hardware
cat docs/bench_safety_checklist.md
cat docs/first_flash_guide.md

# 4. Flash (--execute required for real flash)
# Bootloader must be flashed at 0x08000000, firmware at 0x08008000.
# Bootloader flashing via ST-Link is not yet scripted (flash_stlink.sh
# handles the app only). Use STM32_Programmer_CLI directly:
#   STM32_Programmer_CLI -c port=SWD -d build_bootloader/bootloader.bin 0x08000000 -v
```
