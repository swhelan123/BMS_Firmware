/* ltc6812.h — LTC6812 device driver.
 *
 * SAFETY GUARDS (enforced in ltc6812.c, never bypassed):
 *   1. ltc6812_cell_chain_set_balance()  — only valid on BMS_CHAIN_CELL.
 *      Calling with BMS_CHAIN_TEMP is a fatal programming error (sets FAULT_INTERNAL).
 *   2. ltc6812_temp_chain_set_sensor_bias() — only valid on BMS_CHAIN_TEMP.
 *   3. No public API accepts an arbitrary DCC bitmask and arbitrary chain;
 *      balance and bias operations have separate, chain-specific functions.
 *
 * Register map references: LTC6812 datasheet §7 (Configuration Registers),
 *   §8 (Cell Voltage Registers), §9 (Auxiliary Registers).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bms_types.h"
#include "bms_constants.h"

/* ── Configuration register A layout (from LTC6812 datasheet Table 43) ──── */
typedef struct {
    uint8_t gpio;       /* GPIO1-5 pin control (bits 4:0) */
    uint8_t refon;      /* Reference remains powered between conversions */
    uint8_t adcopt;     /* ADC mode option bit */
    uint16_t vuvcmp;    /* Undervoltage comparison register */
    uint16_t vovcmp;    /* Overvoltage comparison register */
    uint16_t dcc_low;   /* DCC discharge bits, cells 1–12 (only on CELL chain) */
    uint8_t  dcc_high;  /* DCC discharge bits, cells 13–15  (only on CELL chain) */
    uint8_t  dcto;      /* Discharge timer */
} Ltc6812CfgA;

/* Convert threshold mV to comparison register value (datasheet formula).
 * VOVCMP = ceil(VOV × 10 / 16),  VUVCMP = floor(VUV × 10 / 16) - 1. */
static inline uint16_t ltc6812_ov_reg(uint16_t mv)  { return (uint16_t)((mv * 10u + 15u) / 16u); }
static inline uint16_t ltc6812_uv_reg(uint16_t mv)  { return (uint16_t)(mv * 10u / 16u - 1u); }

/* ── Initialisation ──────────────────────────────────────────────────────── */
/* Wake both chains and write safe default configuration registers. */
BmsResult ltc6812_init_chain(BmsChain chain, uint8_t num_ics);

/* ── Cell voltage measurement (CELL chain only) ────────────────────────────  */
/* Start ADC conversion; poll for completion; read all 5 register groups.
 * raw_mv[ic][cell] receives cell voltage in mV (100 µV LSB × raw value / 10).
 * pec_ok[ic] set true if all register groups for that IC had valid PEC.
 * Returns BMS_ERR_PEC if any PEC failed; caller should retry or set fault. */
BmsResult ltc6812_read_cells(BmsChain chain, uint8_t num_ics,
                              uint16_t raw_mv[CELL_IC_COUNT][CELLS_PER_IC],
                              bool pec_ok[CELL_IC_COUNT]);

/* ── Auxiliary / temperature measurement (TEMP chain) ────────────────────── */
/* Start ADAX; poll; read AUX groups A–D (9 channels per IC × num_ics).
 * raw_adc[ic][ch] receives 12-bit ADC result × 100 µV.
 * Caller is responsible for enabling/clearing S-outputs around this call. */
BmsResult ltc6812_read_aux(BmsChain chain, uint8_t num_ics,
                            uint16_t raw_adc[TEMP_IC_COUNT][9],
                            bool pec_ok[TEMP_IC_COUNT]);

/* ── TEMP-chain S-output control (sensor bias) ─────────────────────────────
 * Assert S-outputs for channels specified in s_mask (bit N = channel N, 0-based).
 * MUST be cleared after measurement in success AND error paths.
 * chain MUST be BMS_CHAIN_TEMP; function refuses and returns BMS_ERR_FORBIDDEN
 * if called with BMS_CHAIN_CELL. */
BmsResult ltc6812_temp_chain_set_sensor_bias(BmsChain chain, uint8_t num_ics,
                                              uint16_t s_mask_per_ic);

/* Clear all S-outputs on the TEMP chain. Call after every temp measurement cycle. */
BmsResult ltc6812_temp_chain_clear_s_outputs(BmsChain chain, uint8_t num_ics);

/* ── CELL-chain balance control ───────────────────────────────────────────
 * Set DCC bits for balancing. dcc_mask[ic] bit N = discharge cell N+1.
 * chain MUST be BMS_CHAIN_CELL. Returns BMS_ERR_FORBIDDEN if chain is TEMP.
 * THIS FUNCTION MUST NEVER BE CALLED WITH BMS_CHAIN_TEMP. */
BmsResult ltc6812_cell_chain_set_balance(BmsChain chain, uint8_t num_ics,
                                          const uint16_t dcc_mask[CELL_IC_COUNT]);

/* Clear all DCC (balance) bits on CELL chain. */
BmsResult ltc6812_cell_chain_clear_balance(BmsChain chain, uint8_t num_ics);

/* ── Open-wire detection (CELL chain) ─────────────────────────────────────── */
/* ADOW pull-down pass then pull-up pass; cells where V_PUP - V_PDN > 400 mV
 * are flagged in open_wire_detected[]. chain must be BMS_CHAIN_CELL.
 * Returns BMS_ERR_PEC on any PEC failure during either pass. */
BmsResult ltc6812_run_open_wire(BmsChain chain, uint8_t num_ics,
                                 bool open_wire_detected[TOTAL_CELL_COUNT]);

/* ── Bring-up probe (read CFGA, no conversion, no balance/bias changes) ───── */
/* Wake the chain and read CFGA from each IC. pec_ok[ic] true if PEC valid.
 * cfga_out[ic][6] receives raw register bytes (or zeroes on PEC error).
 * Does NOT start any conversion, write any configuration, or alter CS state
 * beyond the normal SPI transaction end.
 * safe on both CELL and TEMP chains. */
BmsResult ltc6812_probe_chain(BmsChain chain, uint8_t num_ics,
                               bool pec_ok[5],
                               uint8_t cfga_out[5][6]);
