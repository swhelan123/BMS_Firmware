/* bms_protocol.c — UART protocol framing, CRC, and packet dispatch. */
#include "bms_protocol.h"
#include "bms_protocol_ids.h"
#include "bms_config.h"
#include "bms_constants.h"
#include "bms_measurements.h"
#include "bms_faults.h"
#include "bms_outputs.h"
#include "bms_state.h"
#include "bms_diagnostics.h"
#include "bms_balance.h"
#include "bms_soc.h"
#include "ltc6812.h"
#include "board_uart.h"
#include "board_clock.h"
#include "board_adc.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "isl28022.h"
/* board_outputs.h included here only for BmsGpioSnapshot read-only diagnostic.
 * No board_outputs_set_*() calls are made from this file. */
#include "board_outputs.h"
#include <string.h>

/* ── CRC-16/CCITT-FALSE ──────────────────────────────────────────────────── */
uint16_t bms_protocol_crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1u) ^ 0x1021u) : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

/* ── Frame RX state machine ───────────────────────────────────────────────── */
typedef enum {
    RX_SOF0, RX_SOF1, RX_PKT_ID_LO, RX_PKT_ID_HI,
    RX_FLAGS, RX_SEQ, RX_LEN_LO, RX_LEN_HI, RX_PAYLOAD, RX_CRC_LO, RX_CRC_HI
} RxState;

#define RX_BUF_SIZE  (PROTOCOL_MAX_PAYLOAD + FRAME_OVERHEAD)

static struct {
    RxState  state;
    uint8_t  buf[RX_BUF_SIZE];
    uint16_t buf_pos;
    uint16_t pkt_id;
    uint8_t  flags;
    uint8_t  seq;
    uint16_t payload_len;
} s_rx;

static uint8_t s_tx_buf[RX_BUF_SIZE];

/* ── Response helpers ─────────────────────────────────────────────────────── */
static void send_response(uint16_t pkt_id, uint8_t seq,
                           const uint8_t *payload, uint16_t payload_len,
                           bool is_error) {
    uint16_t frame_len = FRAME_OVERHEAD + payload_len;
    s_tx_buf[0] = FRAME_SOF_0;
    s_tx_buf[1] = FRAME_SOF_1;
    s_tx_buf[2] = (uint8_t)(pkt_id & 0xFFu);
    s_tx_buf[3] = (uint8_t)(pkt_id >> 8u);
    s_tx_buf[4] = PROTOCOL_FLAGS_IS_RESPONSE | (is_error ? PROTOCOL_FLAGS_IS_ERROR : 0u);
    s_tx_buf[5] = seq;
    s_tx_buf[6] = (uint8_t)(payload_len & 0xFFu);
    s_tx_buf[7] = (uint8_t)(payload_len >> 8u);
    if (payload && payload_len) {
        memcpy(&s_tx_buf[8], payload, payload_len);
    }
    uint16_t crc = bms_protocol_crc16(s_tx_buf, (uint16_t)(frame_len - 2u));
    s_tx_buf[frame_len - 2u] = (uint8_t)(crc >> 8u);
    s_tx_buf[frame_len - 1u] = (uint8_t)(crc & 0xFFu);
    board_uart_write(s_tx_buf, frame_len);
}

static void send_error(uint16_t pkt_id, uint8_t seq, ProtoError err) {
    uint8_t payload = (uint8_t)err;
    send_response(pkt_id, seq, &payload, 1u, true);
}

/* ── Packet handlers ──────────────────────────────────────────────────────── */
static void handle_get_capabilities(uint8_t seq) {
    uint8_t resp[PKT_CAPABILITIES_RESP_SIZE];
    memset(resp, 0, sizeof(resp));
    resp[0]  = (uint8_t)(FIRMWARE_TYPE_BMS_APP & 0xFFu);
    resp[1]  = (uint8_t)(FIRMWARE_TYPE_BMS_APP >> 8u);
    resp[2]  = FW_VERSION_MAJOR;
    resp[3]  = FW_VERSION_MINOR;
    resp[4]  = FW_VERSION_PATCH;
    resp[5]  = (uint8_t)(HW_PROFILE_ID & 0xFFu);
    resp[6]  = (uint8_t)(HW_PROFILE_ID >> 8u);
    resp[7]  = (uint8_t)(PROTOCOL_VERSION & 0xFFu);
    resp[8]  = (uint8_t)(PROTOCOL_VERSION >> 8u);
    resp[9]  = (uint8_t)(CONFIG_SCHEMA_VERSION & 0xFFu);
    resp[10] = (uint8_t)(CONFIG_SCHEMA_VERSION >> 8u);
    resp[11] = (uint8_t)TOTAL_CELL_COUNT;
    resp[12] = (uint8_t)TOTAL_TEMP_COUNT;
    uint32_t ff = BMS_APP_FEATURE_FLAGS;
    resp[13] = (uint8_t)(ff); resp[14] = (uint8_t)(ff >> 8);
    resp[15] = (uint8_t)(ff >> 16); resp[16] = (uint8_t)(ff >> 24);
    resp[17] = PROTOCOL_MAX_PAYLOAD_LOG2;
    uint32_t app_sz = APP_REGION_SIZE;
    resp[18] = (uint8_t)app_sz; resp[19] = (uint8_t)(app_sz>>8);
    resp[20] = (uint8_t)(app_sz>>16); resp[21] = (uint8_t)(app_sz>>24);
    uint32_t cfg_sz = CONFIG_SLOT_SIZE;
    resp[22] = (uint8_t)cfg_sz; resp[23] = (uint8_t)(cfg_sz>>8);
    resp[24] = (uint8_t)(cfg_sz>>16); resp[25] = (uint8_t)(cfg_sz>>24);
    send_response(PKT_GET_CAPABILITIES, seq, resp, sizeof(resp), false);
}

static void handle_get_values(uint8_t seq) {
    const PackMeasurement *pack  = bms_measurements_get_pack();
    uint8_t resp[PKT_VALUES_RESP_SIZE];
    memset(resp, 0, sizeof(resp));
    uint32_t vbat = (uint32_t)pack->vbat_mv;
    resp[0]=(uint8_t)vbat; resp[1]=(uint8_t)(vbat>>8); resp[2]=(uint8_t)(vbat>>16); resp[3]=(uint8_t)(vbat>>24);
    uint32_t vpk = (uint32_t)pack->vpack_mv;
    resp[4]=(uint8_t)vpk; resp[5]=(uint8_t)(vpk>>8); resp[6]=(uint8_t)(vpk>>16); resp[7]=(uint8_t)(vpk>>24);
    uint32_t ib = (uint32_t)pack->i_batt_ma;
    resp[8]=(uint8_t)ib; resp[9]=(uint8_t)(ib>>8); resp[10]=(uint8_t)(ib>>16); resp[11]=(uint8_t)(ib>>24);
    uint16_t st = (uint16_t)bms_state_get();
    resp[12]=(uint8_t)st; resp[13]=(uint8_t)(st>>8);
    uint64_t af = bms_faults_get_active();
    for (int i=0;i<8;i++) { resp[14+i]=(uint8_t)(af>>(8*i)); }
    uint64_t lf = bms_faults_get_latched();
    for (int i=0;i<8;i++) { resp[22+i]=(uint8_t)(lf>>(8*i)); }
    resp[30] = bms_outputs_get_state();
    uint32_t up = board_clock_get_ms();
    resp[31]=(uint8_t)up; resp[32]=(uint8_t)(up>>8); resp[33]=(uint8_t)(up>>16); resp[34]=(uint8_t)(up>>24);
    uint8_t mf = (pack->vbat_valid?4u:0u)|(pack->vpack_valid?8u:0u);
    resp[35] = mf;
    int16_t soc = bms_soc_get_pct_x10();
    resp[36] = (uint8_t)((uint16_t)soc & 0xFFu);
    resp[37] = (uint8_t)((uint16_t)soc >> 8u);
    send_response(PKT_GET_VALUES, seq, resp, sizeof(resp), false);
}

static void handle_get_cells(uint8_t seq, const uint8_t *payload, uint16_t len) {
    bool include_validity = (len > 0 && (payload[0] & 0x01u));
    const CellSnapshot *cells = bms_measurements_get_cells();
    uint16_t resp_len = include_validity ? PKT_GET_CELLS_RESP_FULL : PKT_GET_CELLS_RESP_BASE;
    uint8_t resp[PKT_GET_CELLS_RESP_FULL];
    memset(resp, 0, sizeof(resp));
    resp[0] = TOTAL_CELL_COUNT & 0xFFu;
    resp[1] = (TOTAL_CELL_COUNT >> 8u) & 0xFFu;
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        uint16_t mv = cells->mv[i];
        resp[2 + i*2]   = (uint8_t)(mv & 0xFFu);
        resp[2 + i*2+1] = (uint8_t)(mv >> 8u);
    }
    uint32_t ts = cells->timestamp_ms;
    resp[152]=(uint8_t)ts; resp[153]=(uint8_t)(ts>>8); resp[154]=(uint8_t)(ts>>16); resp[155]=(uint8_t)(ts>>24);
    if (include_validity) {
        for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
            if (cells->valid[i]) { resp[156 + i/8] |= (uint8_t)(1u << (i%8u)); }
        }
    }
    send_response(PKT_GET_CELLS, seq, resp, resp_len, false);
}

static void handle_get_temps(uint8_t seq) {
    const TempSnapshot *temps = bms_measurements_get_temps();
    uint8_t resp[PKT_GET_TEMPS_RESP_SIZE];
    memset(resp, 0, sizeof(resp));
    resp[0] = TOTAL_TEMP_COUNT & 0xFFu;
    resp[1] = 0u;
    for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
        int16_t t = temps->cx10[i];
        resp[2 + i*2]   = (uint8_t)((uint16_t)t & 0xFFu);
        resp[2 + i*2+1] = (uint8_t)((uint16_t)t >> 8u);
    }
    send_response(PKT_GET_TEMPS, seq, resp, sizeof(resp), false);
}

static void handle_get_faults(uint8_t seq) {
    uint8_t resp[16];
    uint64_t af = bms_faults_get_active();
    uint64_t lf = bms_faults_get_latched();
    for (int i=0;i<8;i++) { resp[i]=(uint8_t)(af>>(8*i)); }
    for (int i=0;i<8;i++) { resp[8+i]=(uint8_t)(lf>>(8*i)); }
    send_response(PKT_GET_FAULTS, seq, resp, sizeof(resp), false);
}

static void handle_get_config(uint8_t seq) {
    const BmsConfig *cfg = bms_config_get();
    send_response(PKT_GET_CONFIG, seq, (const uint8_t *)cfg, CONFIG_SCHEMA_SIZE, false);
}

static void handle_validate_config(uint8_t seq, const uint8_t *payload, uint16_t len) {
    if (len != CONFIG_SCHEMA_SIZE) { send_error(PKT_VALIDATE_CONFIG, seq, PROTO_ERR_BAD_LENGTH); return; }
    uint16_t err_off;
    uint8_t resp[3] = {0, 0xFF, 0xFF};
    BmsResult r = bms_config_validate((const BmsConfig *)payload, &err_off);
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    resp[1] = (uint8_t)(err_off & 0xFFu);
    resp[2] = (uint8_t)(err_off >> 8u);
    send_response(PKT_VALIDATE_CONFIG, seq, resp, sizeof(resp), false);
}

static void handle_set_config_ram(uint8_t seq, const uint8_t *payload, uint16_t len) {
    if (len != CONFIG_SCHEMA_SIZE) { send_error(PKT_SET_CONFIG_RAM, seq, PROTO_ERR_BAD_LENGTH); return; }
    uint16_t err_off;
    BmsResult r = bms_config_apply_ram((const BmsConfig *)payload);
    uint8_t resp[3] = {(r==BMS_OK)?0u:1u, 0xFF, 0xFF};
    if (r != BMS_OK) {
        bms_config_validate((const BmsConfig *)payload, &err_off);
        resp[1] = (uint8_t)(err_off & 0xFFu);
        resp[2] = (uint8_t)(err_off >> 8u);
    }
    send_response(PKT_SET_CONFIG_RAM, seq, resp, sizeof(resp), false);
}

static void handle_store_config(uint8_t seq, const uint8_t *payload, uint16_t len) {
    if (len != CONFIG_SCHEMA_SIZE) { send_error(PKT_STORE_CONFIG, seq, PROTO_ERR_BAD_LENGTH); return; }
    BmsResult r = bms_config_store((const BmsConfig *)payload);
    uint8_t resp = (r == BMS_OK) ? 0u : (uint8_t)PROTO_ERR_CONFIG_INVALID;
    send_response(PKT_STORE_CONFIG, seq, &resp, 1u, r != BMS_OK);
}

static void handle_clear_latched_faults(uint8_t seq, const uint8_t *payload, uint16_t len) {
    uint64_t mask = UINT64_MAX; /* default: attempt to clear all */
    if (len >= 8) {
        mask = 0;
        for (int i = 0; i < 8; i++) { mask |= ((uint64_t)payload[i] << (8 * i)); }
    }
    uint64_t cleared = bms_faults_clear_latched(mask);
    uint8_t resp[8];
    for (int i = 0; i < 8; i++) { resp[i] = (uint8_t)(cleared >> (8 * i)); }
    send_response(PKT_CLEAR_LATCHED_FAULTS, seq, resp, sizeof(resp), false);
}

static void handle_get_diagnostics_summary(uint8_t seq) {
    const BmsDiagnostics *diag = bms_diagnostics_get();
    /* Layout: reset_cause(1) + pec_cell(4) + pec_temp(4) + i2c(4) +
     *          open_wire_valid(1) + open_wire_mask(10) + uptime_ms(4) = 28 bytes */
    uint8_t resp[28];
    memset(resp, 0, sizeof(resp));
    resp[0] = diag->reset_cause;
    resp[1] = (uint8_t)(diag->pec_errors_cell);
    resp[2] = (uint8_t)(diag->pec_errors_cell >> 8);
    resp[3] = (uint8_t)(diag->pec_errors_cell >> 16);
    resp[4] = (uint8_t)(diag->pec_errors_cell >> 24);
    resp[5] = (uint8_t)(diag->pec_errors_temp);
    resp[6] = (uint8_t)(diag->pec_errors_temp >> 8);
    resp[7] = (uint8_t)(diag->pec_errors_temp >> 16);
    resp[8] = (uint8_t)(diag->pec_errors_temp >> 24);
    resp[9]  = (uint8_t)(diag->i2c_errors);
    resp[10] = (uint8_t)(diag->i2c_errors >> 8);
    resp[11] = (uint8_t)(diag->i2c_errors >> 16);
    resp[12] = (uint8_t)(diag->i2c_errors >> 24);
    resp[13] = diag->open_wire_valid ? 1u : 0u;
    /* Pack open_wire_detected[75] into 10 bytes (75 bits). */
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        if (diag->open_wire_detected[i]) {
            resp[14 + i / 8u] |= (uint8_t)(1u << (i % 8u));
        }
    }
    uint32_t up = diag->uptime_ms;
    resp[24] = (uint8_t)up; resp[25] = (uint8_t)(up>>8);
    resp[26] = (uint8_t)(up>>16); resp[27] = (uint8_t)(up>>24);
    send_response(PKT_GET_DIAGNOSTICS_SUMMARY, seq, resp, sizeof(resp), false);
}

static void handle_run_openwire(uint8_t seq) {
    bool detected[TOTAL_CELL_COUNT];
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, detected);
    bool valid = (r == BMS_OK);
    bms_diagnostics_set_open_wire(valid, detected);
    if (valid) {
        /* Latch FAULT_BIT_CELL_OPENWIRE if any required cell is open —
         * the on-demand scan must have the same safety effect as the
         * periodic scan in the main loop. */
        bms_faults_apply_openwire(detected, bms_config_get());
    }
    /* Response: status(1) + open_wire_mask(10) = 11 bytes */
    uint8_t resp[11];
    memset(resp, 0, sizeof(resp));
    resp[0] = valid ? 0u : 1u;
    if (valid) {
        for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
            if (detected[i]) { resp[1 + i / 8u] |= (uint8_t)(1u << (i % 8u)); }
        }
    }
    send_response(PKT_RUN_OPENWIRE, seq, resp, sizeof(resp), false);
}

static void handle_get_boot_info(uint8_t seq) {
    const BmsDiagnostics *diag = bms_diagnostics_get();
    /* Layout: fw_type(2) + major(1) + minor(1) + patch(1) + hw_profile_id(2) +
     *          proto_version(2) + reset_cause(1) + uptime_ms(4) = 14 bytes */
    uint8_t resp[14];
    resp[0]  = (uint8_t)(FIRMWARE_TYPE_BMS_APP & 0xFFu);
    resp[1]  = (uint8_t)(FIRMWARE_TYPE_BMS_APP >> 8u);
    resp[2]  = FW_VERSION_MAJOR;
    resp[3]  = FW_VERSION_MINOR;
    resp[4]  = FW_VERSION_PATCH;
    resp[5]  = (uint8_t)(HW_PROFILE_ID & 0xFFu);
    resp[6]  = (uint8_t)(HW_PROFILE_ID >> 8u);
    resp[7]  = (uint8_t)(PROTOCOL_VERSION & 0xFFu);
    resp[8]  = (uint8_t)(PROTOCOL_VERSION >> 8u);
    resp[9]  = diag->reset_cause;
    uint32_t up = diag->uptime_ms;
    resp[10] = (uint8_t)up; resp[11] = (uint8_t)(up>>8);
    resp[12] = (uint8_t)(up>>16); resp[13] = (uint8_t)(up>>24);
    send_response(PKT_GET_BOOT_INFO, seq, resp, sizeof(resp), false);
}

static void handle_enter_bootloader(uint8_t seq, const uint8_t *payload, uint16_t len) {
    if (len < 4) { send_error(PKT_ENTER_BOOTLOADER, seq, PROTO_ERR_BAD_LENGTH); return; }
    uint32_t magic = (uint32_t)payload[0] | ((uint32_t)payload[1]<<8) |
                     ((uint32_t)payload[2]<<16) | ((uint32_t)payload[3]<<24);
    if (magic != BL_ENTRY_FLAG) { send_error(PKT_ENTER_BOOTLOADER, seq, PROTO_ERR_BAD_STATE); return; }
    /* Ack before reset */
    send_response(PKT_ENTER_BOOTLOADER, seq, NULL, 0u, false);
    bms_state_request_bootloader_entry();
}

/* ── Bring-up / bench-diagnostic handlers ─────────────────────────────────── */

static void handle_get_gpio_snapshot(uint8_t seq) {
    BmsGpioSnapshot snap;
    board_outputs_get_gpio_snapshot(&snap);
    uint8_t resp[9];
    resp[0] = snap.cs_cell;
    resp[1] = snap.cs_temp;
    resp[2] = snap.power_button;
    resp[3] = snap.charge_detect;
    resp[4] = snap.power_enable;
    resp[5] = snap.master_ok_raw;
    resp[6] = snap.discharge_raw;
    resp[7] = snap.charge_raw;
    resp[8] = snap.charger_safety_raw;
    send_response(PKT_GET_GPIO_SNAPSHOT, seq, resp, sizeof(resp), false);
}

static void handle_get_outputs_snapshot(uint8_t seq) {
    BmsGpioSnapshot snap;
    board_outputs_get_gpio_snapshot(&snap);
    uint8_t logical = bms_outputs_get_state();
    uint8_t raw = (uint8_t)(
        ((uint8_t)(snap.master_ok_raw    & 1u)      ) |
        ((uint8_t)(snap.discharge_raw    & 1u) << 1u) |
        ((uint8_t)(snap.charge_raw       & 1u) << 2u) |
        ((uint8_t)(snap.charger_safety_raw & 1u) << 3u)
    );
    uint8_t resp[2] = {logical, raw};
    send_response(PKT_GET_OUTPUTS_SNAPSHOT, seq, resp, sizeof(resp), false);
}

static void handle_probe_cell_chain(uint8_t seq) {
    bool pec_ok[CELL_IC_COUNT];
    uint8_t cfga_out[CELL_IC_COUNT][LTC6812_REG_GROUP_BYTES];
    BmsResult r = ltc6812_probe_chain(BMS_CHAIN_CELL, CELL_IC_COUNT, pec_ok, cfga_out);

    BmsChainProbeResult probe;
    memset(&probe, 0, sizeof(probe));
    probe.run         = true;
    probe.result      = r;
    probe.ic_count    = CELL_IC_COUNT;
    probe.timestamp_ms = board_clock_get_ms();
    for (uint8_t ic = 0; ic < CELL_IC_COUNT; ic++) {
        probe.ic[ic].responded = pec_ok[ic];
        if (pec_ok[ic]) { memcpy(probe.ic[ic].cfga, cfga_out[ic], LTC6812_REG_GROUP_BYTES); }
    }
    bms_diagnostics_store_cell_probe(&probe);

    uint8_t resp[2u + CELL_IC_COUNT * 7u];
    memset(resp, 0, sizeof(resp));
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    resp[1] = CELL_IC_COUNT;
    for (uint8_t ic = 0; ic < CELL_IC_COUNT; ic++) {
        uint8_t *p = &resp[2u + ic * 7u];
        p[0] = pec_ok[ic] ? 1u : 0u;
        if (pec_ok[ic]) { memcpy(&p[1], cfga_out[ic], LTC6812_REG_GROUP_BYTES); }
    }
    send_response(PKT_PROBE_CELL_CHAIN, seq, resp, sizeof(resp), false);
}

static void handle_probe_temp_chain(uint8_t seq) {
    bool pec_ok[TEMP_IC_COUNT];
    uint8_t cfga_out[TEMP_IC_COUNT][LTC6812_REG_GROUP_BYTES];
    BmsResult r = ltc6812_probe_chain(BMS_CHAIN_TEMP, TEMP_IC_COUNT, pec_ok, cfga_out);

    BmsChainProbeResult probe;
    memset(&probe, 0, sizeof(probe));
    probe.run         = true;
    probe.result      = r;
    probe.ic_count    = TEMP_IC_COUNT;
    probe.timestamp_ms = board_clock_get_ms();
    for (uint8_t ic = 0; ic < TEMP_IC_COUNT; ic++) {
        probe.ic[ic].responded = pec_ok[ic];
        if (pec_ok[ic]) { memcpy(probe.ic[ic].cfga, cfga_out[ic], LTC6812_REG_GROUP_BYTES); }
    }
    bms_diagnostics_store_temp_probe(&probe);

    uint8_t resp[2u + TEMP_IC_COUNT * 7u];
    memset(resp, 0, sizeof(resp));
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    resp[1] = TEMP_IC_COUNT;
    for (uint8_t ic = 0; ic < TEMP_IC_COUNT; ic++) {
        uint8_t *p = &resp[2u + ic * 7u];
        p[0] = pec_ok[ic] ? 1u : 0u;
        if (pec_ok[ic]) { memcpy(&p[1], cfga_out[ic], LTC6812_REG_GROUP_BYTES); }
    }
    send_response(PKT_PROBE_TEMP_CHAIN, seq, resp, sizeof(resp), false);
}

static void handle_probe_isl28022(uint8_t seq) {
    uint8_t reg_buf[2] = {0u, 0u};
    BmsResult r = board_i2c_read_reg(ISL28022_I2C_ADDR, ISL28022_REG_CONFIG, reg_buf, 2u);

    BmsIslProbeResult probe;
    probe.run         = true;
    probe.result      = r;
    probe.config_reg  = (r == BMS_OK)
                      ? (uint16_t)(((uint16_t)reg_buf[0] << 8u) | reg_buf[1])
                      : 0xFFFFu;
    probe.timestamp_ms = board_clock_get_ms();
    bms_diagnostics_store_isl_probe(&probe);

    uint8_t resp[3] = {(r == BMS_OK) ? 0u : 1u, reg_buf[0], reg_buf[1]};
    send_response(PKT_PROBE_ISL28022, seq, resp, sizeof(resp), false);
}

static void handle_read_vpack_raw(uint8_t seq) {
    uint16_t raw = 0u;
    BmsResult r = board_adc_read_raw(&raw);

    BmsVpackRawResult probe;
    probe.run         = true;
    probe.result      = r;
    probe.raw_code    = (r == BMS_OK) ? raw : 0u;
    probe.timestamp_ms = board_clock_get_ms();
    bms_diagnostics_store_vpack_raw(&probe);

    uint8_t resp[3];
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    resp[1] = (uint8_t)(raw & 0xFFu);
    resp[2] = (uint8_t)(raw >> 8u);
    send_response(PKT_READ_VPACK_RAW, seq, resp, sizeof(resp), false);
}

static void handle_balance_disable_all(uint8_t seq) {
    bms_balance_disable_all();
    uint8_t resp = 0u;
    send_response(PKT_BALANCE_DISABLE_ALL, seq, &resp, 1u, false);
}

/* ── One-shot measurement handlers ───────────────────────────────────────────
 * Each triggers a full measurement cycle, then packs the resulting snapshot.
 * Response layout:
 *   MEASURE_CELLS_ONCE: status(1) + cell_count(2) + mv[75][2](150) + ts(4) + valid_bits(10) = 167
 *   MEASURE_TEMPS_ONCE: status(1) + temp_count(2) + cx10[75][2](150) + ts(4)               = 157
 *   MEASURE_POWER_ONCE: status(1) + vbat_mv(4) + vpack_mv(4) + i_batt_ma(4) + flags(1) + ts(4) = 18
 */

#define MEASURE_CELLS_RESP_SIZE  (1u + PKT_GET_CELLS_RESP_FULL)  /* 167 */
#define MEASURE_TEMPS_RESP_SIZE  (1u + PKT_GET_TEMPS_RESP_SIZE + 4u) /* 157 */
#define MEASURE_POWER_RESP_SIZE  (18u)

static void handle_measure_cells_once(uint8_t seq) {
    BmsResult r = bms_measurements_run_cell_cycle();
    const CellSnapshot *cells = bms_measurements_get_cells();

    uint8_t resp[MEASURE_CELLS_RESP_SIZE];
    memset(resp, 0, sizeof(resp));
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    resp[1] = (uint8_t)(TOTAL_CELL_COUNT & 0xFFu);
    resp[2] = (uint8_t)((TOTAL_CELL_COUNT >> 8u) & 0xFFu);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        resp[3u + i*2u]   = (uint8_t)(cells->mv[i] & 0xFFu);
        resp[3u + i*2u+1u] = (uint8_t)(cells->mv[i] >> 8u);
    }
    uint32_t ts = cells->timestamp_ms;
    resp[153u] = (uint8_t)ts;       resp[154u] = (uint8_t)(ts >> 8u);
    resp[155u] = (uint8_t)(ts >> 16u); resp[156u] = (uint8_t)(ts >> 24u);
    for (uint8_t i = 0; i < TOTAL_CELL_COUNT; i++) {
        if (cells->valid[i]) { resp[157u + i / 8u] |= (uint8_t)(1u << (i % 8u)); }
    }
    send_response(PKT_MEASURE_CELLS_ONCE, seq, resp, sizeof(resp), false);
}

static void handle_measure_temps_once(uint8_t seq) {
    BmsResult r = bms_measurements_run_temp_cycle();
    const TempSnapshot *temps = bms_measurements_get_temps();

    uint8_t resp[MEASURE_TEMPS_RESP_SIZE];
    memset(resp, 0, sizeof(resp));
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    resp[1] = (uint8_t)(TOTAL_TEMP_COUNT & 0xFFu);
    resp[2] = 0u;
    for (uint8_t i = 0; i < TOTAL_TEMP_COUNT; i++) {
        uint16_t raw = (uint16_t)temps->cx10[i];
        resp[3u + i*2u]   = (uint8_t)(raw & 0xFFu);
        resp[3u + i*2u+1u] = (uint8_t)(raw >> 8u);
    }
    uint32_t ts = temps->timestamp_ms;
    resp[153u] = (uint8_t)ts;        resp[154u] = (uint8_t)(ts >> 8u);
    resp[155u] = (uint8_t)(ts >> 16u); resp[156u] = (uint8_t)(ts >> 24u);
    send_response(PKT_MEASURE_TEMPS_ONCE, seq, resp, sizeof(resp), false);
}

static void handle_measure_power_once(uint8_t seq) {
    BmsResult r = bms_measurements_run_pack_cycle();
    const PackMeasurement *pack = bms_measurements_get_pack();

    uint8_t resp[MEASURE_POWER_RESP_SIZE];
    memset(resp, 0, sizeof(resp));
    resp[0] = (r == BMS_OK) ? 0u : 1u;
    uint32_t vbat = (uint32_t)pack->vbat_mv;
    resp[1] = (uint8_t)vbat; resp[2] = (uint8_t)(vbat>>8u);
    resp[3] = (uint8_t)(vbat>>16u); resp[4] = (uint8_t)(vbat>>24u);
    uint32_t vpk = (uint32_t)pack->vpack_mv;
    resp[5] = (uint8_t)vpk; resp[6] = (uint8_t)(vpk>>8u);
    resp[7] = (uint8_t)(vpk>>16u); resp[8] = (uint8_t)(vpk>>24u);
    uint32_t ib = (uint32_t)pack->i_batt_ma;
    resp[9] = (uint8_t)ib; resp[10] = (uint8_t)(ib>>8u);
    resp[11] = (uint8_t)(ib>>16u); resp[12] = (uint8_t)(ib>>24u);
    resp[13] = (pack->vbat_valid   ? 1u : 0u) |
               (pack->vpack_valid  ? 2u : 0u) |
               (pack->i_batt_valid ? 4u : 0u);
    uint32_t ts = pack->timestamp_ms;
    resp[14] = (uint8_t)ts; resp[15] = (uint8_t)(ts>>8u);
    resp[16] = (uint8_t)(ts>>16u); resp[17] = (uint8_t)(ts>>24u);
    send_response(PKT_MEASURE_POWER_ONCE, seq, resp, sizeof(resp), false);
}

/* ── Frame dispatch ───────────────────────────────────────────────────────── */
static void dispatch_packet(uint16_t pkt_id, uint8_t seq,
                             const uint8_t *payload, uint16_t payload_len) {
    switch (pkt_id) {
        case PKT_GET_CAPABILITIES:   handle_get_capabilities(seq); break;
        case PKT_GET_VALUES:         handle_get_values(seq); break;
        case PKT_GET_CELLS:          handle_get_cells(seq, payload, payload_len); break;
        case PKT_GET_TEMPS:          handle_get_temps(seq); break;
        case PKT_GET_FAULTS:              handle_get_faults(seq); break;
        case PKT_CLEAR_LATCHED_FAULTS:    handle_clear_latched_faults(seq, payload, payload_len); break;
        case PKT_GET_CONFIG:              handle_get_config(seq); break;
        case PKT_VALIDATE_CONFIG:         handle_validate_config(seq, payload, payload_len); break;
        case PKT_SET_CONFIG_RAM:          handle_set_config_ram(seq, payload, payload_len); break;
        case PKT_STORE_CONFIG:            handle_store_config(seq, payload, payload_len); break;
        case PKT_GET_DIAGNOSTICS_SUMMARY: handle_get_diagnostics_summary(seq); break;
        case PKT_RUN_OPENWIRE:            handle_run_openwire(seq); break;
        case PKT_GET_GPIO_SNAPSHOT:       handle_get_gpio_snapshot(seq); break;
        case PKT_GET_OUTPUTS_SNAPSHOT:    handle_get_outputs_snapshot(seq); break;
        case PKT_PROBE_CELL_CHAIN:        handle_probe_cell_chain(seq); break;
        case PKT_PROBE_TEMP_CHAIN:        handle_probe_temp_chain(seq); break;
        case PKT_PROBE_ISL28022:          handle_probe_isl28022(seq); break;
        case PKT_READ_VPACK_RAW:          handle_read_vpack_raw(seq); break;
        case PKT_BALANCE_DISABLE_ALL:     handle_balance_disable_all(seq); break;
        case PKT_MEASURE_CELLS_ONCE:      handle_measure_cells_once(seq); break;
        case PKT_MEASURE_TEMPS_ONCE:      handle_measure_temps_once(seq); break;
        case PKT_MEASURE_POWER_ONCE:      handle_measure_power_once(seq); break;
        case PKT_GET_BOOT_INFO:           handle_get_boot_info(seq); break;
        case PKT_ENTER_BOOTLOADER:        handle_enter_bootloader(seq, payload, payload_len); break;
        /* Bootloader update packets not supported in application firmware */
        case PKT_BOOT_UPDATE_BEGIN:
        case PKT_BOOT_UPDATE_CHUNK:
        case PKT_BOOT_UPDATE_FINALIZE:
        case PKT_BOOT_UPDATE_ABORT:
            send_error(pkt_id, seq, PROTO_ERR_NOT_SUPPORTED);
            break;
        default:
            send_error(pkt_id, seq, PROTO_ERR_UNKNOWN_PACKET);
            break;
    }
}

/* ── RX tick ──────────────────────────────────────────────────────────────── */
void bms_protocol_init(void) {
    s_rx.state = RX_SOF0;
    s_rx.buf_pos = 0;
}

void bms_protocol_tick(void) {
    while (board_uart_rx_ready()) {
        uint8_t b = board_uart_read_byte();
        switch (s_rx.state) {
            case RX_SOF0:
                if (b == FRAME_SOF_0) {
                    s_rx.buf[0] = b; s_rx.buf_pos = 1;
                    s_rx.state = RX_SOF1;
                }
                break;
            case RX_SOF1:
                if (b == FRAME_SOF_1) { s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_PKT_ID_LO; }
                else { s_rx.state = RX_SOF0; }
                break;
            case RX_PKT_ID_LO:
                s_rx.pkt_id = b; s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_PKT_ID_HI; break;
            case RX_PKT_ID_HI:
                s_rx.pkt_id |= (uint16_t)b << 8u; s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_FLAGS; break;
            case RX_FLAGS:
                s_rx.flags = b; s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_SEQ; break;
            case RX_SEQ:
                s_rx.seq = b; s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_LEN_LO; break;
            case RX_LEN_LO:
                s_rx.payload_len = b; s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_LEN_HI; break;
            case RX_LEN_HI:
                s_rx.payload_len |= (uint16_t)b << 8u; s_rx.buf[s_rx.buf_pos++] = b;
                if (s_rx.payload_len > PROTOCOL_MAX_PAYLOAD) {
                    s_rx.state = RX_SOF0; /* frame too large, discard */
                } else {
                    s_rx.state = (s_rx.payload_len > 0) ? RX_PAYLOAD : RX_CRC_LO;
                }
                break;
            case RX_PAYLOAD:
                s_rx.buf[s_rx.buf_pos++] = b;
                if (s_rx.buf_pos >= (uint16_t)(FRAME_OVERHEAD - 2u + s_rx.payload_len)) {
                    s_rx.state = RX_CRC_LO;
                }
                break;
            case RX_CRC_LO:
                s_rx.buf[s_rx.buf_pos++] = b; s_rx.state = RX_CRC_HI; break;
            case RX_CRC_HI: {
                s_rx.buf[s_rx.buf_pos++] = b;
                uint16_t frame_len = s_rx.buf_pos;
                uint16_t recv_crc  = ((uint16_t)s_rx.buf[frame_len-2] << 8u) | s_rx.buf[frame_len-1];
                uint16_t calc_crc  = bms_protocol_crc16(s_rx.buf, (uint16_t)(frame_len - 2u));
                if (recv_crc == calc_crc) {
                    const uint8_t *payload = &s_rx.buf[FRAME_OVERHEAD - 2u];
                    dispatch_packet(s_rx.pkt_id, s_rx.seq, payload, s_rx.payload_len);
                } else {
                    send_error(s_rx.pkt_id, s_rx.seq, PROTO_ERR_BAD_CRC);
                }
                s_rx.state = RX_SOF0;
                break;
            }
        }
    }
}
