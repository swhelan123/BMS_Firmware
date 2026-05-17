cmake_minimum_required(VERSION 3.20)

project(BmsBootloader C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Source files ──────────────────────────────────────────────────────────────
set(BL_SOURCES
    src/main.c
    src/bl_validate.c
    src/bl_jump.c
    src/bl_flash.c
    src/bl_uart.c
    src/bl_protocol.c
)

# Reuse the startup and system files from the firmware vendor tree
set(STARTUP_SOURCES
    ../firmware/vendor/STM32F3xx/Source/Templates/gcc/startup_stm32f303xc.s
)
set(SYSTEM_SOURCES
    ../firmware/vendor/STM32F3xx/Source/Templates/system_stm32f3xx.c
)

# ── Include paths ─────────────────────────────────────────────────────────────
set(INCLUDE_DIRS
    include
    src
    ../firmware/vendor/CMSIS/Core/Include
    ../firmware/vendor/STM32F3xx/Include
)

# ── Compiler flags ────────────────────────────────────────────────────────────
set(COMMON_COMPILE_FLAGS
    -Wall -Wextra -Werror
    -Wno-unused-parameter
    -ffunction-sections -fdata-sections
    -fno-common
    -Os
)

# ── Linker flags ──────────────────────────────────────────────────────────────
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/stm32f303vc_bootloader.ld")

set(LINKER_FLAGS
    "-T${LINKER_SCRIPT}"
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/bootloader.map
    -Wl,--print-memory-usage
)

# ── Bootloader target ─────────────────────────────────────────────────────────
add_executable(bms_bootloader.elf
    ${STARTUP_SOURCES}
    ${SYSTEM_SOURCES}
    ${BL_SOURCES}
)

target_include_directories(bms_bootloader.elf PRIVATE ${INCLUDE_DIRS})

target_compile_definitions(bms_bootloader.elf PRIVATE STM32F303xC)

target_compile_options(bms_bootloader.elf PRIVATE ${COMMON_COMPILE_FLAGS})

target_link_options(bms_bootloader.elf PRIVATE ${LINKER_FLAGS})

# Post-build: generate .bin and .hex; print size
add_custom_command(TARGET bms_bootloader.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bms_bootloader.elf>
            ${CMAKE_BINARY_DIR}/bootloader.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:bms_bootloader.elf>
            ${CMAKE_BINARY_DIR}/bootloader.hex
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:bms_bootloader.elf>
    COMMENT "Generating bootloader.bin, bootloader.hex"
)

set_target_properties(bms_bootloader.elf PROPERTIES
    LINK_DEPENDS "${LINKER_SCRIPT}")
