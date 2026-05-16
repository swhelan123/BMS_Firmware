/* mock_protocol_deps.c — stubs for bms_protocol.c external dependencies (host tests). */
#include "bms_measurements.h"
#include "bms_diagnostics.h"
#include "bms_state.h"
#include "board_i2c.h"
#include "board_adc.h"
#include "bms_types.h"
#include <string.h>
#include <stdint.h>

static CellSnapshot    s_cells;
static TempSnapshot    s_temps;
static PackMeasurement s_pack;
static BmsState        s_state = BMS_STATE_STANDBY;
static BmsDiagnostics  s_diag;

const CellSnapshot    *bms_measurements_get_cells(void) { return &s_cells; }
const TempSnapshot    *bms_measurements_get_temps(void) { return &s_temps; }
const PackMeasurement *bms_measurements_get_pack(void)  { return &s_pack; }

BmsResult bms_measurements_run_cell_cycle(void) { return BMS_OK; }
BmsResult bms_measurements_run_temp_cycle(void) { return BMS_OK; }
BmsResult bms_measurements_run_pack_cycle(void) { return BMS_OK; }
void bms_measurements_update_pack(int32_t vbat_mv, int32_t vpack_mv,
                                   int32_t i_batt_ma,
                                   bool vbat_valid, bool vpack_valid,
                                   bool i_batt_valid) {
    s_pack.vbat_mv      = vbat_mv;
    s_pack.vpack_mv     = vpack_mv;
    s_pack.i_batt_ma    = i_batt_ma;
    s_pack.vbat_valid   = vbat_valid;
    s_pack.vpack_valid  = vpack_valid;
    s_pack.i_batt_valid = i_batt_valid;
}

BmsState bms_state_get(void)                     { return s_state; }
void     bms_state_request_bootloader_entry(void) {}

const BmsDiagnostics *bms_diagnostics_get(void) { return &s_diag; }
void bms_diagnostics_set_open_wire(bool valid, const bool detected[TOTAL_CELL_COUNT]) {
    s_diag.open_wire_valid = valid;
    if (valid) {
        memcpy(s_diag.open_wire_detected, detected, TOTAL_CELL_COUNT * sizeof(bool));
    }
}
void bms_diagnostics_store_cell_probe(const BmsChainProbeResult *r) { s_diag.cell_probe = *r; }
void bms_diagnostics_store_temp_probe(const BmsChainProbeResult *r) { s_diag.temp_probe = *r; }
void bms_diagnostics_store_isl_probe(const BmsIslProbeResult *r)    { s_diag.isl_probe  = *r; }
void bms_diagnostics_store_vpack_raw(const BmsVpackRawResult *r)    { s_diag.vpack_raw  = *r; }

/* I2C and ADC stubs — return success with zeroed data */
BmsResult board_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t *buf, uint8_t len) {
    (void)dev_addr; (void)reg_addr;
    for (uint8_t i = 0; i < len; i++) { buf[i] = 0u; }
    return BMS_OK;
}
BmsResult board_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr,
                               const uint8_t *data, uint8_t len) {
    (void)dev_addr; (void)reg_addr; (void)data; (void)len;
    return BMS_OK;
}
BmsResult board_adc_read_raw(uint16_t *raw_out) {
    *raw_out = 2048u;
    return BMS_OK;
}
