# 06 — Flash Map and Bootloader

## 1. STM32F303 Flash Overview

| Parameter | Value |
|---|---|
| Flash start | 0x08000000 |
| Flash page size | 2 KB (2048 bytes) |
| Minimum variant flash | 256 KB (128 pages) → 0x08000000–0x0803FFFF |
| Target variant | **OPEN QUESTION: confirm exact STM32F303 variant (CC/VC/RD/RE)**. Sizes below assume 256KB minimum; mark 512KB as bonus. |
| SRAM start | 0x20000000 |

---

## 2. Flash Map (Provisional — 256 KB)

```
Address          Size     Pages   Region             Notes
0x08000000       32 KB    16      BOOTLOADER          Bootloader code + vector table
0x08008000       188 KB   94      APPLICATION         BMS firmware image
0x08037000       8 KB     4       CONFIG_A            Config slot A (dual-slot)
0x08039000       8 KB     4       CONFIG_B            Config slot B (dual-slot)
0x0803B000       20 KB    10      (unallocated)       Reserved for future use

Total: 256 KB
```

> **PROVISIONAL:** All addresses subject to confirmation of STM32F303 variant.
> **OPEN QUESTION HV-11:** If variant has 512 KB flash, application region can expand significantly; a dedicated update-staging region becomes feasible.

Note: An UPDATE_STAGING and DIAGNOSTICS_LOG region were considered in early design but are not present in the current linker scripts. All update transfers are held in RAM; the old image is erased only after full transfer and CRC verification.

For 512 KB variant, proposed expansion:
```
0x08000000       32 KB    16      BOOTLOADER
0x08008000       432 KB   216     APPLICATION
0x0807A000       8 KB     4       CONFIG_A
0x0807C000       8 KB     4       CONFIG_B
0x0807E000       8 KB     4       UPDATE_STAGING / DIAGNOSTICS
0x0807F800       2 KB     1       RESERVED
```

---

## 3. Bootloader Region

- Occupies the first 32 KB of flash (first 16 pages)
- Vector table at 0x08000000; `VTOR = 0x08000000`
- Application vector table at `APP_START_ADDR = 0x08008000`; bootloader must set `VTOR = 0x08008000` before jumping
- Bootloader must **never** erase its own region (enforced by checking erase address range)
- Bootloader stack: top of SRAM (0x20000000 + SRAM_SIZE)

---

## 4. Application Region

- Application vector table at 0x08008000
- Stack pointer at `app_image[0]` (word 0 of vector table); must be in SRAM range
- Reset handler at `app_image[1]` (word 1 of vector table); must be in application flash range with bit 0 set (Thumb)
- Application image checksum (CRC32) covers words 2..N of the image (see package format)

---

## 5. Config Storage Region

- Two slots: CONFIG_A (0x08037000) and CONFIG_B (0x08039000), 8 KB each
- Each slot holds one `BmsConfig` struct (226 bytes) plus padding to the next flash page
- Dual-slot selection algorithm: see `docs/05_config_schema.md §9`
- Config write: erase target slot → write config → verify readback CRC → update metadata
- Config region is **NOT** erased during firmware update (by design)
- Bootloader checks: if config slot CRC invalid after flash update, config remains as-is (it was preserved)

---

## 6. Boot Decision Flow

```
RESET
  │
  ▼
Bootloader starts (always at 0x08000000)
  │
  ├─ Check retained SRAM boot flag:
  │    BL_ENTRY_FLAG == 0xB007B007 ?
  │    Yes → clear flag → stay in bootloader (tool-requested entry)
  │
  ├─ Validate application image:
  │    - SP valid (in SRAM range)?
  │    - Reset vector valid (in APP flash range, bit0 set)?
  │    - App image CRC32 matches stored value?
  │    All pass → jump to application
  │    Any fail → stay in bootloader
  │
  └─ Bootloader loop: expose identity + await update packets
```

---

## 7. App-to-Bootloader Entry Flow

1. Tool sends `ENTER_BOOTLOADER` with magic `0xB007B007`
2. Application firmware:
   a. Deasserts all permissions
   b. Disables IWDG (or sets it to long timeout if not stoppable)
   c. Writes `BL_ENTRY_FLAG = 0xB007B007` to retained SRAM (RTC backup registers or designated SRAM)
   d. Triggers soft reset (`NVIC_SystemReset()`)
3. Bootloader reads flag, clears it, stays in bootloader mode
4. Tool detects reconnect; sends `GET_CAPABILITIES` to confirm bootloader mode
5. Proceeds with `BOOT_UPDATE_BEGIN`

> **OPEN QUESTION:** Which retained RAM/backup register is used for the boot flag. RTC backup registers (RTC_BKP0R) are preferred on STM32F303 as they survive software reset but not power-off. Confirm availability.

---

## 8. Firmware Package Format

The firmware package is a binary file with a fixed header followed by the raw application image.

### Package Header (64 bytes, little-endian)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 4 | pkg_magic | uint32_t | 0xBF00BF00 |
| 4 | 2 | pkg_version | uint16_t | Package format version; currently 1 |
| 6 | 2 | hw_profile_id | uint16_t | Must match device HW_PROFILE_ID |
| 8 | 4 | target_mcu_id | uint32_t | STM32F303 MCU ID (from DBGMCU_IDCODE register; OPEN QUESTION: exact value) |
| 12 | 1 | image_type | uint8_t | 0x01 = application, 0x02 = bootloader update |
| 13 | 3 | reserved_type | uint8_t[3] | 0x00 |
| 16 | 4 | app_start_addr | uint32_t | Must match APP_START_ADDR (0x08008000) |
| 20 | 4 | app_size | uint32_t | Size of payload in bytes |
| 24 | 4 | app_crc32 | uint32_t | CRC32 of payload bytes |
| 28 | 3 | fw_version | uint8_t[3] | [major, minor, patch] of firmware in this package |
| 31 | 1 | reserved_fw | uint8_t | 0x00 |
| 32 | 3 | min_bootloader_version | uint8_t[3] | Minimum bootloader version required |
| 35 | 1 | reserved_bl | uint8_t | 0x00 |
| 36 | 2 | required_config_schema | uint16_t | Config schema version this firmware expects |
| 38 | 2 | reserved_schema | uint16_t | 0x0000 |
| 40 | 4 | pkg_header_crc32 | uint32_t | CRC32 of bytes 0..39 |
| 44 | 20 | reserved | uint8_t[20] | All 0x00 |

Total header: 64 bytes. Payload immediately follows at offset 64.

### Package File Layout

```
[Header: 64 bytes][Payload: app_size bytes]
```

### Package CRC Strategy

- `pkg_header_crc32` covers header bytes 0–39 (before the CRC field)
- `app_crc32` covers payload bytes 0..app_size-1
- Bootloader validates header CRC first, then payload CRC after full transfer

---

## 9. Package Validation Rules (Bootloader)

Bootloader rejects the package and returns `ERR_PACKAGE_INVALID` if:

1. `pkg_magic` != 0xBF00BF00
2. `pkg_version` > `BOOTLOADER_MAX_SUPPORTED_PKG_VERSION`
3. `hw_profile_id` != `HW_PROFILE_ID`
4. `target_mcu_id` != read-back DBGMCU_IDCODE (OPEN QUESTION: confirm register address and expected value)
5. `image_type` is not 0x01 (application)
6. `app_start_addr` != `APP_START_ADDR`
7. `app_size` == 0 or `app_size` > `APP_REGION_SIZE`
8. `pkg_header_crc32` mismatch
9. `fw_version` is below minimum acceptable (policy: no version enforcement in v1; OPEN QUESTION)
10. `required_config_schema` > current `CONFIG_SCHEMA_VERSION` on device (warn; not reject in v1)
11. After full transfer: `app_crc32` computed over received bytes does not match header value
12. Application image sanity checks:
    - `image[0]` (stack pointer) not in `[SRAM_START, SRAM_START + SRAM_SIZE]`
    - `image[1]` (reset vector) not in `[APP_START_ADDR, APP_START_ADDR + app_size]` with bit 0 set

---

## 10. Vector Table Checks

Before jumping to application, bootloader verifies:

```c
uint32_t sp = *(uint32_t*)APP_START_ADDR;
uint32_t rv = *(uint32_t*)(APP_START_ADDR + 4);

assert(sp >= SRAM_START && sp <= (SRAM_START + SRAM_SIZE));
assert((rv & ~1U) >= APP_START_ADDR && (rv & ~1U) < (APP_START_ADDR + APP_MAX_SIZE));
```

If either check fails, bootloader does not jump; logs error and remains in bootloader mode.

---

## 11. Config Preservation Policy

| Scenario | Config After Update |
|---|---|
| Normal firmware update via bootloader | Config preserved (bootloader does not touch config region) |
| Factory reset (explicit command) | Config erased; device boots to safe defaults |
| Bootloader update itself | Config preserved |
| New firmware requires higher schema version | Device will have `FAULT_CONFIG_INVALID` until new config is written by tool |
| Flash corruption in config region | Safe defaults used; `FAULT_CONFIG_INVALID` set |

The tool must warn the user when `required_config_schema` in the package differs from the device's current `config_schema_version`. It must prompt the user to re-validate and re-store config after update.

---

## 12. Power-Loss Behaviour

| Stage | Behaviour on Power Loss |
|---|---|
| Before `BOOT_UPDATE_BEGIN` accepted | No change; previous firmware intact |
| During chunk transfer (before all chunks received) | Chunks held in RAM; old flash image untouched; boot flag not set; old image intact; bootloader stays in BL mode on next boot |
| During flash erase (old image partially erased) | Old image invalid; bootloader stays in BL mode; waits for retry |
| During flash write (new image partially written) | CRC will fail on next boot; bootloader stays in BL mode |
| After `BOOT_UPDATE_FINALIZE` with CRC pass | New image valid; boots on next reset |
| Config region at any stage | Unchanged; both slots either both valid or one valid from before |

Chunks are accumulated in RAM during transfer. Erase of the old image begins only after all chunks are received and the assembled payload CRC is verified. Power loss before that point leaves the old image intact and the bootloader retries on the next connection. For 512 KB variants a dedicated on-flash staging region would enable full image validation before erase.

---

## 13. Development ST-Link Flashing

Development flashing bypasses the bootloader entirely. The ST-Link / STM32_Programmer_CLI tool writes directly to flash.

**Application flash (typical):** `scripts/flash_stlink.sh --app firmware.bin` writes to 0x08008000. Bootloader at 0x08000000 is untouched.

**Bootloader flash:** `scripts/flash_stlink.sh --bootloader bl.bin --execute` writes to 0x08000000. Requires `--execute` as an explicit acknowledgement.

The desktop tool's Firmware Flash page:
- Generates and displays the ST-Link command (dry-run mode)
- Executes the flash only when the safety checkbox is ticked (`--execute` equivalent)
- Does NOT bypass package validation for the protocol update path (only the ST-Link path bypasses validation)

---

## 14. Rollback / Recovery Limitations (v1)

- **No automatic rollback** in v1. If new firmware is written and does not boot, the only recovery path is ST-Link reflash.
- **Partial mitigation:** Bootloader stays in bootloader mode if new image CRC fails; tool can retry update.
- **No A/B firmware partitions** in v1 (not enough flash on 256KB variant). Mark as future enhancement for 512KB or external flash variants.
- Consider adding a "boot counter" that the application must clear within N seconds of boot; if it doesn't clear (crash loop), bootloader could refuse to boot the new image and wait for retry. **OPEN QUESTION: implement or defer?**
