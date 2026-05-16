/* bms_protocol_ids.h — packet ID constants.
 * Generated from: protocol/packet_ids.yaml
 * Do not edit manually — regenerate with: scripts/generate_protocol.py
 */
#pragma once
#include <stdint.h>

/* ── Packet IDs ───────────────────────────────────────────────────────────── */
#define PKT_GET_CAPABILITIES        ((uint16_t)0x0001u)
#define PKT_GET_VALUES              ((uint16_t)0x0101u)
#define PKT_GET_CELLS               ((uint16_t)0x0102u)
#define PKT_GET_TEMPS               ((uint16_t)0x0103u)
#define PKT_GET_FAULTS              ((uint16_t)0x0104u)
#define PKT_CLEAR_LATCHED_FAULTS    ((uint16_t)0x0105u)
#define PKT_GET_CONFIG              ((uint16_t)0x0201u)
#define PKT_VALIDATE_CONFIG         ((uint16_t)0x0202u)
#define PKT_SET_CONFIG_RAM          ((uint16_t)0x0203u)
#define PKT_STORE_CONFIG            ((uint16_t)0x0204u)
#define PKT_GET_DIAGNOSTICS_SUMMARY ((uint16_t)0x0301u)
#define PKT_GET_DIAGNOSTICS_LOG     ((uint16_t)0x0302u)
#define PKT_RUN_OPENWIRE            ((uint16_t)0x0303u)
/* Bring-up / bench diagnostics (0x0304–0x030A) */
#define PKT_GET_GPIO_SNAPSHOT       ((uint16_t)0x0304u)
#define PKT_GET_OUTPUTS_SNAPSHOT    ((uint16_t)0x0305u)
#define PKT_PROBE_CELL_CHAIN        ((uint16_t)0x0306u)
#define PKT_PROBE_TEMP_CHAIN        ((uint16_t)0x0307u)
#define PKT_PROBE_ISL28022          ((uint16_t)0x0308u)
#define PKT_READ_VPACK_RAW          ((uint16_t)0x0309u)
#define PKT_BALANCE_DISABLE_ALL     ((uint16_t)0x030Au)
/* One-shot measurement commands (0x030B–0x030D) */
#define PKT_MEASURE_CELLS_ONCE      ((uint16_t)0x030Bu)
#define PKT_MEASURE_TEMPS_ONCE      ((uint16_t)0x030Cu)
#define PKT_MEASURE_POWER_ONCE      ((uint16_t)0x030Du)
#define PKT_GET_BOOT_INFO           ((uint16_t)0x0401u)
#define PKT_ENTER_BOOTLOADER        ((uint16_t)0x0402u)
#define PKT_BOOT_UPDATE_BEGIN       ((uint16_t)0x0403u)
#define PKT_BOOT_UPDATE_CHUNK       ((uint16_t)0x0404u)
#define PKT_BOOT_UPDATE_FINALIZE    ((uint16_t)0x0405u)
#define PKT_BOOT_UPDATE_ABORT       ((uint16_t)0x0406u)

/* ── Fixed response payload sizes ────────────────────────────────────────── */
#define PKT_CAPABILITIES_RESP_SIZE  (26u)
#define PKT_VALUES_RESP_SIZE        (36u)
#define PKT_GET_CELLS_RESP_BASE     (156u)  /* without validity bitfield */
#define PKT_GET_CELLS_RESP_FULL     (166u)  /* with 10-byte validity bitfield */
#define PKT_GET_TEMPS_RESP_SIZE     (152u)  /* 2 + 75*2 */
#define PKT_GET_FAULTS_RESP_SIZE    (16u)   /* active(8) + latched(8) */

/* ── Frame constants ──────────────────────────────────────────────────────── */
#define FRAME_SOF_0     (0xAAu)
#define FRAME_SOF_1     (0x55u)
#define FRAME_OVERHEAD  (10u)   /* SOF(2) PKT_ID(2) FLAGS(1) SEQ(1) LEN(2) CRC(2) */
