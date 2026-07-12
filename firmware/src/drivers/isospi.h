/* isospi.h — isoSPI transport layer for LTC6812 daisy chains.
 *
 * Provides command-level read/write to an LTC68xx daisy chain via
 * an LTC6820 master bridge and SPI1.
 *
 * PEC-15: every 6-byte register group is followed by 2 PEC bytes computed
 * with polynomial 0x4599 as specified in the LTC6812 datasheet.
 *
 * Chain orientation (isospi_reverse): OPEN QUESTION — confirm from schematic
 * whether the LTC6820 is wired ISOSPI_FORWARD or ISOSPI_REVERSE for each chain.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bms_types.h"
#include "bms_constants.h"

/* LTC68xx command opcodes (from LTC6812 datasheet Table 35) */
#define LTC_CMD_WRCFGA      (0x0001u)  /* Write Configuration Register Group A */
#define LTC_CMD_RDCFGA      (0x0002u)  /* Read Configuration Register Group A  */
#define LTC_CMD_WRCFGB      (0x0024u)  /* Write Configuration Register Group B */
#define LTC_CMD_RDCFGB      (0x0026u)  /* Read Configuration Register Group B  */
#define LTC_CMD_RDCVA       (0x0004u)  /* Read Cell Voltage Register Group A   */
#define LTC_CMD_RDCVB       (0x0006u)  /* Read Cell Voltage Register Group B   */
#define LTC_CMD_RDCVC       (0x0008u)  /* Read Cell Voltage Register Group C   */
#define LTC_CMD_RDCVD       (0x000Au)  /* Read Cell Voltage Register Group D   */
#define LTC_CMD_RDCVE       (0x0009u)  /* Read Cell Voltage Register Group E   */
#define LTC_CMD_RDAUXA      (0x000Cu)  /* Read Auxiliary Register Group A      */
#define LTC_CMD_RDAUXB      (0x000Eu)  /* Read Auxiliary Register Group B      */
#define LTC_CMD_RDAUXC      (0x000Du)  /* Read Auxiliary Register Group C      */
#define LTC_CMD_RDAUXD      (0x000Fu)  /* Read Auxiliary Register Group D      */
#define LTC_CMD_ADCV        (0x0360u)  /* Start Cell Voltage ADC (all ch, 7kHz, DCP=0) */
#define LTC_CMD_ADCV_DCP    (0x0370u)  /* Same, DCP=1 (discharge stays on during
                                        * conversion) — used for the temp chain so
                                        * the sensor bias switch is not opened while
                                        * the sensor voltage is being sampled */
#define LTC_CMD_ADAX        (0x0560u)  /* Start Aux ADC (all GPIO, 7kHz)       */
#define LTC_CMD_ADOW_PDN    (0x0328u)  /* Open-Wire, pull-down (PUP=0), MD=10 normal 7kHz */
#define LTC_CMD_ADOW_PUP    (0x0368u)  /* Open-Wire, pull-up  (PUP=1), MD=10 normal 7kHz.
                                        * MD must match ADCV timing: previous MD=00 (422Hz)
                                        * encoding took ~12.8ms/conversion — longer than the
                                        * fixed 4ms wait, so registers were read mid-conversion. */
#define LTC_CMD_CLRCELL     (0x0711u)  /* Clear Cell Voltage registers         */
#define LTC_CMD_CLRAUX      (0x0712u)  /* Clear Aux registers                  */
#define LTC_CMD_CLRSTAT     (0x0713u)  /* Clear Status registers               */
#define LTC_CMD_PLADC       (0x0714u)  /* Poll ADC conversion status           */
#define LTC_CMD_DIAGN       (0x0715u)  /* Diagnose MUX and check memory        */
#define LTC_CMD_WRCOMM      (0x0721u)  /* Write COMM Register Group            */
#define LTC_CMD_RDCOMM      (0x0722u)  /* Read COMM Register Group             */
#define LTC_CMD_STCOMM      (0x0723u)  /* Start I2C/SPI Communication          */
#define LTC_CMD_MUTE        (0x0028u)  /* Mute discharge                       */
#define LTC_CMD_UNMUTE      (0x0029u)  /* Un-mute discharge                    */

/* Maximum ICs on one chain (used for buffer sizing) */
#define ISOSPI_MAX_ICS  (5u)

/* ── PEC-15 ──────────────────────────────────────────────────────────────── */
/* Compute PEC-15 over len bytes of data. Polynomial 0x4599. */
uint16_t isospi_pec15(const uint8_t *data, uint8_t len);

/* ── Wakeup ───────────────────────────────────────────────────────────────── */
/* Send wakeup pulses on the given chain. Must be called before any command
 * after a period of inactivity (>1.8 ms idle). */
void isospi_wakeup(BmsChain chain);

/* ── Low-level command framing ────────────────────────────────────────────── */
/* Send a broadcast command with no data payload (e.g., ADCV, CLRCELL). */
BmsResult isospi_cmd_broadcast(BmsChain chain, uint16_t cmd);

/* Write one register group (6 bytes) to all num_ics devices.
 * data must contain num_ics × 6 bytes, ordered from last IC to first IC. */
BmsResult isospi_write_all(BmsChain chain, uint16_t cmd,
                           const uint8_t *data, uint8_t num_ics);

/* Read one register group from all num_ics devices.
 * data receives num_ics × 6 bytes (ordered from last IC to first IC).
 * pec_ok_per_ic[i] is set true if PEC matched for IC i (0 = last in chain). */
BmsResult isospi_read_all(BmsChain chain, uint16_t cmd,
                          uint8_t *data, uint8_t num_ics,
                          bool *pec_ok_per_ic);

/* Read one byte from SDO after sending a broadcast command (used for PLADC polling).
 * out must be a 1-element buffer. CS is asserted and deasserted around the transfer. */
void isospi_read_byte_after_cmd(BmsChain chain, uint8_t *out);
