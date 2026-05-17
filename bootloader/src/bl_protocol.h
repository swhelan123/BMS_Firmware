/* bl_protocol.h — Bootloader UART protocol handler.
 *
 * Implements the BMS framing protocol (SOF 0xAA 0x55, CRC-16/CCITT-FALSE, big-endian CRC)
 * for the bootloader-side packet IDs:
 *   0x0001  GET_CAPABILITIES
 *   0x0401  GET_BOOT_INFO
 *   0x0403  BOOT_UPDATE_BEGIN
 *   0x0404  BOOT_UPDATE_CHUNK
 *   0x0405  BOOT_UPDATE_FINALIZE
 *   0x0406  BOOT_UPDATE_ABORT
 *
 * In BMS_HOST_BUILD the UART calls are replaced by bl_protocol_process_frame(),
 * which lets unit tests drive the state machine directly.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum frame payload the bootloader will accept (512 bytes covers 256-byte chunk
 * plus the 8-byte chunk header). */
#define BL_MAX_PAYLOAD   512u

/* Chunk size the bootloader advertises to the host. */
#define BL_CHUNK_SIZE    256u

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Blocking protocol loop: receives frames from UART and dispatches them.
 * Never returns in the hardware build.  In host build, not used (use
 * bl_protocol_process_frame instead). */
void bl_protocol_run(void);

/* ── Host-build test interface (BMS_HOST_BUILD only) ─────────────────────── */
#ifdef BMS_HOST_BUILD

/* Reset the update state machine — call between test cases. */
void bl_protocol_reset_ctx(void);

/* Process one raw frame (wire bytes including SOF and CRC).
 * Returns a malloc-free byte slice written into out_buf (caller supplies buffer
 * of at least BL_MAX_PAYLOAD + 10 bytes).  *out_len is set to frame length.
 * Returns true if a response was produced, false on framing / CRC error. */
bool bl_protocol_process_frame(const uint8_t *frame, uint16_t frame_len,
                               uint8_t *out_buf, uint16_t *out_len);

#endif /* BMS_HOST_BUILD */
