#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

CFLAGS="-std=c11 -Wall -Wextra -Wno-unused-parameter -DBMS_HOST_BUILD=1"
INCLUDES="-Ifirmware/include -Ifirmware/src/board -Ifirmware/src/drivers -Ifirmware/src/bms -Ifirmware/src -Ibootloader/include -Ibootloader/src -Itests/vendor/unity -Itests/mock_bsp"

FW_SRCS=(
    firmware/src/bms/bms_config.c
    firmware/src/bms/bms_faults.c
    firmware/src/bms/bms_outputs.c
    firmware/src/bms/bms_balance.c
    firmware/src/drivers/isospi.c
    firmware/src/drivers/ltc6812.c
)

MOCK_SRCS=(
    tests/mock_bsp/mock_board_spi.c
    tests/mock_bsp/mock_board_uart.c
    tests/mock_bsp/mock_board_outputs.c
    tests/mock_bsp/mock_board_clock.c
    tests/mock_bsp/mock_board_flash.c
)

UNITY=tests/vendor/unity/unity.c
PASS=0; FAIL=0

run_test() {
    local name="$1"
    local src="$2"
    shift 2
    echo "==> Building $name ..."
    if clang $CFLAGS $INCLUDES "${FW_SRCS[@]}" "${MOCK_SRCS[@]}" "$UNITY" "$src" "$@" -o "build_tests/$name" 2>&1; then
        echo "    Running ..."
        if "build_tests/$name"; then
            PASS=$((PASS+1))
        else
            FAIL=$((FAIL+1))
        fi
    else
        echo "    BUILD FAILED"
        FAIL=$((FAIL+1))
    fi
    echo ""
}

run_test test_pec15            tests/unit/test_pec15.c

# test_measurements_decode and test_enepaq_table: both link bms_measurements.c
# via mock_meas_deps.c (not ltc6812.c — mock provides all driver stubs).
MEAS_SRCS_COMMON=(
    firmware/src/bms/bms_measurements.c
    tests/mock_bsp/mock_board_clock.c
    tests/mock_bsp/mock_meas_deps.c
    tests/vendor/unity/unity.c
)

echo "==> Building test_measurements_decode ..."
if clang $CFLAGS $INCLUDES "${MEAS_SRCS_COMMON[@]}" \
        tests/unit/test_measurements_decode.c \
        -o build_tests/test_measurements_decode 2>&1; then
    echo "    Running ..."
    if build_tests/test_measurements_decode; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
    fi
else
    echo "    BUILD FAILED"
    FAIL=$((FAIL+1))
fi
echo ""

echo "==> Building test_enepaq_table ..."
if clang $CFLAGS $INCLUDES "${MEAS_SRCS_COMMON[@]}" \
        tests/unit/test_enepaq_table.c \
        -o build_tests/test_enepaq_table 2>&1; then
    echo "    Running ..."
    if build_tests/test_enepaq_table; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
    fi
else
    echo "    BUILD FAILED"
    FAIL=$((FAIL+1))
fi
echo ""
run_test test_protocol_crc     tests/unit/test_protocol_crc.c \
    firmware/src/bms/bms_protocol.c \
    tests/mock_bsp/mock_protocol_deps.c
run_test test_config_validate  tests/unit/test_config_validate.c
run_test test_config_masks     tests/unit/test_config_masks.c
run_test test_bms_outputs      tests/unit/test_bms_outputs.c
run_test test_flash_layout     tests/unit/test_flash_layout.c
run_test test_bootloader_validate \
    tests/unit/test_bootloader_validate.c \
    bootloader/src/bl_validate.c \
    bootloader/src/bl_jump.c
run_test test_faults           tests/unit/test_faults.c
run_test test_balance          tests/unit/test_balance.c
run_test test_openwire         tests/unit/test_openwire.c

# test_isl28022: links isl28022.c against mock_board_i2c.c (no real I2C hardware).
run_test test_isl28022 \
    tests/unit/test_isl28022.c \
    firmware/src/drivers/isl28022.c \
    tests/mock_bsp/mock_board_i2c.c

run_test test_bootloader_protocol \
    tests/unit/test_bootloader_protocol.c \
    bootloader/src/bl_protocol.c \
    bootloader/src/bl_flash.c \
    bootloader/src/bl_validate.c \
    bootloader/src/bl_jump.c

echo "==================================="
echo "C Unit Tests: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] && exit 0 || exit 1
