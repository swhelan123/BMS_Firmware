cmake_minimum_required(VERSION 3.20)

# ── Project ──────────────────────────────────────────────────────────────────
project(BmsFirmware C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(BMS_BUILD_TESTS "Build host-compiled unit tests (native)" OFF)

# ── Source files ──────────────────────────────────────────────────────────────
set(BOARD_SOURCES
    src/board/board_clock.c
    src/board/board_outputs.c
    src/board/board_inputs.c
    src/board/board_uart.c
    src/board/board_spi.c
    src/board/board_i2c.c
    src/board/board_adc.c
    src/board/board_can.c
    src/board/board_flash.c
)

set(DRIVER_SOURCES
    src/drivers/isospi.c
    src/drivers/ltc6812.c
    src/drivers/isl28022.c
)

set(BMS_SOURCES
    src/bms/bms_config.c
    src/bms/bms_measurements.c
    src/bms/bms_faults.c
    src/bms/bms_outputs.c
    src/bms/bms_state.c
    src/bms/bms_balance.c
    src/bms/bms_protocol.c
    src/bms/bms_diagnostics.c
    src/bms/bms_can.c
    src/bms/bms_charger.c
    src/bms/bms_soc.c
    src/bms_main_loop.c
    src/main.c
)

set(STARTUP_SOURCES
    vendor/STM32F3xx/Source/Templates/gcc/startup_stm32f303xc.s
)

set(SYSTEM_SOURCES
    vendor/STM32F3xx/Source/Templates/system_stm32f3xx.c
)

# ── Include paths ──────────────────────────────────────────────────────────────
set(INCLUDE_DIRS
    include
    src/board
    src/drivers
    src/bms
    vendor/CMSIS/Core/Include
    vendor/STM32F3xx/Include
)

# ── Compiler flags ─────────────────────────────────────────────────────────────
set(COMMON_COMPILE_FLAGS
    -Wall -Wextra -Werror
    -Wno-unused-parameter        # many stubs; remove when fully implemented
    -ffunction-sections -fdata-sections
    -fno-common
    -Os                          # optimise for size
)

# ── Linker flags ───────────────────────────────────────────────────────────────
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/stm32f303vc_bms.ld")

set(LINKER_FLAGS
    "-T${LINKER_SCRIPT}"
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/firmware.map
    -Wl,--print-memory-usage
)

# ── Firmware target ────────────────────────────────────────────────────────────
if(NOT BMS_BUILD_TESTS)
    add_executable(bms_firmware.elf
        ${STARTUP_SOURCES}
        ${SYSTEM_SOURCES}
        ${BOARD_SOURCES}
        ${DRIVER_SOURCES}
        ${BMS_SOURCES}
    )

    target_include_directories(bms_firmware.elf PRIVATE ${INCLUDE_DIRS})

    target_compile_definitions(bms_firmware.elf PRIVATE STM32F303xC)

    target_compile_options(bms_firmware.elf PRIVATE ${COMMON_COMPILE_FLAGS})

    target_link_options(bms_firmware.elf PRIVATE ${LINKER_FLAGS})

    # Post-build: generate .bin and .hex; print size
    add_custom_command(TARGET bms_firmware.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bms_firmware.elf>
                ${CMAKE_BINARY_DIR}/firmware.bin
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:bms_firmware.elf>
                ${CMAKE_BINARY_DIR}/firmware.hex
        COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:bms_firmware.elf>
        COMMENT "Generating firmware.bin, firmware.hex"
    )

    set_target_properties(bms_firmware.elf PROPERTIES
        LINK_DEPENDS "${LINKER_SCRIPT}")
endif()

# ── Unit tests (native host build) ────────────────────────────────────────────
if(BMS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests tests_build)
endif()
