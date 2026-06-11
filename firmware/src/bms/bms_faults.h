/* bms_faults.h — fault evaluation and latching. */
#pragma once
#include <stdint.h>
#include "bms_types.h"
#include "bms_config.h"

/* Evaluate all fault conditions from current measurements.
 * Updates internal active and latched fault bitmaps.
 * Must be called after every measurement cycle. */
void bms_faults_evaluate(const CellSnapshot    *cells,
                          const TempSnapshot    *temps,
                          const PackMeasurement *pack,
                          const BmsConfig       *cfg);

/* Report a PEC error on the given chain. Escalates to ISOSPI fault after
 * LTC6812_MAX_RETRIES consecutive errors. */
void bms_faults_report_pec_error(BmsChain chain);

/* Clear the PEC consecutive error counter and active ISOSPI fault for a chain. */
void bms_faults_clear_pec_counter(BmsChain chain);

/* Report an I2C error on the ISL28022 bus. */
void bms_faults_report_i2c_error(void);

/* Clear the active I2C fault (call after a successful I2C transaction). */
void bms_faults_clear_i2c_error(void);

/* Returns current active fault bitmap (conditions present now). */
uint64_t bms_faults_get_active(void);

/* Returns latched fault bitmap (sticky until explicitly cleared). */
uint64_t bms_faults_get_latched(void);

/* Attempt to clear latched faults in mask. Only clears faults whose active
 * condition has resolved. Returns bitmask of faults actually cleared. */
uint64_t bms_faults_clear_latched(uint64_t mask);

/* Set a specific fault bit directly (for internal error reporting). */
void bms_faults_set(FaultBit bit);

/* Latch a fault bit without marking it active. For historical conditions
 * (e.g. IWDG reset detected at boot) that must block permissions until
 * explicitly cleared via PKT_CLEAR_LATCHED_FAULTS. */
void bms_faults_set_latched(FaultBit bit);

/* Apply an open-wire scan result: sets/clears FAULT_BIT_CELL_OPENWIRE based
 * on whether any cell in required_cell_mask is flagged open. Latches per
 * fault_bits.yaml. Call only with a scan that completed successfully. */
void bms_faults_apply_openwire(const bool detected[TOTAL_CELL_COUNT],
                                const BmsConfig *cfg);
