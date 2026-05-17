/* bl_protocol.c — BMS bootloader protocol state machine.
 *
 * Frame wire format (matches tool/src/protocol/framing.py exactly):
 *   SOF[2]       0xAA 0x55
 *   PKT_ID[2]    uint16 LE
 *   FLAGS[1]     bit0=IS_RESPONSE  bit1=IS_ERROR
 *   SEQ[1]
 *   PAYLOAD_LEN[2]  uint16 LE
 *   PAYLOAD[N]
 *   CRC[2]       CRC-16/CCITT-FALSE over SOF..PAYLOAD, BIG-ENDIAN
 *
 * Update state machine:
 *   IDLE → BEGIN → RECEIVING_CHUNKS → (FINALIZE or ABORT) → IDLE
 *
 * Flash write strategy: pages are erased on first halfword write to each 2 KB
 * page boundary; no pre-erase pass.  Power loss during update leaves the app
 * invalid and the bootloader stays resident on the next reset.
 */
#include "bl_protocol.h"
#include "bl_flash.h"
#include "bl_validate.h"
#include "bl_config.h"
#include "bl_uart.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Frame constants ─────────────────────────────────────────────────────── */
#define SOF0          0xAAu
#define SOF1          0x55u
#define FRAME_OVERHEAD 10u  /* SOF(2)+ID(2)+FLAGS(1)+SEQ(1)+LEN(2)+CRC(2) */

#define FLAG_IS_RESPONSE 0x01u
#define FLAG_IS_ERROR    0x02u

/* ── Packet IDs ──────────────────────────────────────────────────────────── */
#define PKT_GET_CAPABILITIES   0x0001u
#define PKT_GET_BOOT_INFO      0x0401u
#define PKT_BOOT_UPDATE_BEGIN  0x0403u
#define PKT_BOOT_UPDATE_CHUNK  0x0404u
#define PKT_BOOT_UPDATE_FINALIZE 0x0405u
#define PKT_BOOT_UPDATE_ABORT  0x0406u

/* ── Status codes in response payloads ───────────────────────────────────── */
#define BL_RESP_OK         0x00u
#define BL_RESP_ERR        0x01u

/* Reject reason codes for BEGIN */
#define BL_REJECT_BAD_HEADER   0x01u
#define BL_REJECT_BAD_MCU      0x02u
#define BL_REJECT_BAD_PROFILE  0x03u
#define BL_REJECT_BAD_BL_VER   0x04u
#define BL_REJECT_BAD_SIZE     0x05u

/* Firmware type for bootloader identity */
#define FIRMWARE_TYPE_BOOTLOADER 0x0002u

/* ── CRC-16/CCITT-FALSE ──────────────────────────────────────────────────── */
static uint16_t crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0u; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u)
                ? (uint16_t)((crc << 1u) ^ 0x1021u)
                : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}

/* ── Frame encoder ───────────────────────────────────────────────────────── */
static uint16_t encode_frame(uint8_t *out, uint16_t pkt_id, uint8_t seq,
                              bool is_error,
                              const uint8_t *payload, uint16_t plen) {
    out[0] = SOF0;
    out[1] = SOF1;
    out[2] = (uint8_t)(pkt_id & 0xFFu);
    out[3] = (uint8_t)((pkt_id >> 8u) & 0xFFu);
    out[4] = FLAG_IS_RESPONSE | (is_error ? FLAG_IS_ERROR : 0u);
    out[5] = seq;
    out[6] = (uint8_t)(plen & 0xFFu);
    out[7] = (uint8_t)((plen >> 8u) & 0xFFu);
    if (plen > 0u && payload != NULL) {
        memcpy(&out[8], payload, plen);
    }
    uint16_t c = crc16(out, (uint16_t)(8u + plen));
    out[8u + plen]     = (uint8_t)(c >> 8u);
    out[9u + plen]     = (uint8_t)(c & 0xFFu);
    return (uint16_t)(FRAME_OVERHEAD + plen);
}

/* ── Helpers to write LE integers into byte buffers ─────────────────────── */
static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}

/* ── Update context ──────────────────────────────────────────────────────── */
typedef enum {
    UPD_IDLE,
    UPD_RECEIVING,
} UpdateState;

typedef struct {
    UpdateState  state;
    uint32_t     app_size;       /* from BEGIN header */
    uint32_t     total_chunks;
    uint32_t     next_chunk;     /* next expected chunk index */
    uint32_t     bytes_written;
    uint32_t     erased_up_to;   /* flash addr through which pages are erased */
} BlUpdateCtx;

static BlUpdateCtx s_update_ctx;

/* Response buffer used by the hardware UART loop (not needed in host build). */
#ifndef BMS_HOST_BUILD
static uint8_t s_resp_buf[FRAME_OVERHEAD + BL_MAX_PAYLOAD + 8u];
#endif

void bl_protocol_reset_ctx(void) {
    memset(&s_update_ctx, 0, sizeof(s_update_ctx));
    s_update_ctx.state = UPD_IDLE;
}

/* ── Packet handlers ─────────────────────────────────────────────────────── */

static uint16_t handle_get_capabilities(uint8_t seq,
                                        uint8_t *out) {
    uint8_t pl[26];
    put_u16le(&pl[0],  FIRMWARE_TYPE_BOOTLOADER);
    pl[2] = BL_VERSION_MAJOR;
    pl[3] = BL_VERSION_MINOR;
    pl[4] = BL_VERSION_PATCH;
    put_u16le(&pl[5],  HW_PROFILE_ID);
    put_u16le(&pl[7],  BL_PROTOCOL_VERSION);
    put_u16le(&pl[9],  BL_CONFIG_SCHEMA_VERSION);
    pl[11] = 0u;  /* cell_count — bootloader does not measure */
    pl[12] = 0u;  /* temp_count */
    put_u32le(&pl[13], 0u);  /* feature_flags — none in bootloader */
    pl[17] = 9u;  /* max_payload_log2 = 512 bytes */
    put_u32le(&pl[18], APP_REGION_SIZE);
    put_u32le(&pl[22], CONFIG_SLOT_SIZE);
    return encode_frame(out, PKT_GET_CAPABILITIES, seq, false, pl, 26u);
}

static uint16_t handle_get_boot_info(uint8_t seq, uint8_t *out) {
    uint8_t pl[8];
    pl[0] = BL_VERSION_MAJOR;
    pl[1] = BL_VERSION_MINOR;
    pl[2] = BL_VERSION_PATCH;
    pl[3] = BL_PROTOCOL_VERSION;
    put_u32le(&pl[4], BL_ENTRY_FLAG);  /* magic that triggers entry */
    return encode_frame(out, PKT_GET_BOOT_INFO, seq, false, pl, 8u);
}

/* Erase pages in [APP_START_ADDR, APP_START_ADDR + app_size) lazily.
 * Erases up to and including the page containing target_addr. */
static BlFlashResult ensure_erased_through(uint32_t target_addr) {
    while (s_update_ctx.erased_up_to <= target_addr) {
        BlFlashResult r = bl_flash_erase_page(s_update_ctx.erased_up_to);
        if (r != BL_FLASH_OK) { return r; }
        s_update_ctx.erased_up_to += FLASH_PAGE_SIZE;
    }
    return BL_FLASH_OK;
}

static uint16_t handle_update_begin(uint8_t seq,
                                    const uint8_t *payload, uint16_t plen,
                                    uint8_t *out) {
    uint8_t pl[10];

    if (plen < (uint16_t)sizeof(FirmwarePackageHeader)) {
        pl[0] = BL_RESP_ERR;
        pl[1] = BL_REJECT_BAD_HEADER;
        put_u32le(&pl[2], 0u);
        put_u32le(&pl[6], 0u);
        return encode_frame(out, PKT_BOOT_UPDATE_BEGIN, seq, true, pl, 10u);
    }

    const FirmwarePackageHeader *hdr = (const FirmwarePackageHeader *)payload;

#ifndef BMS_HOST_BUILD
    volatile uint32_t *idcode = (volatile uint32_t *)DBGMCU_IDCODE_ADDR;
    uint32_t mcu_dev_id = *idcode & DBGMCU_DEV_ID_MASK;
#else
    uint32_t mcu_dev_id = STM32F303VC_DEV_ID;  /* simulate correct MCU in host tests */
#endif

    BlValidateResult vr = bl_validate_package_header(hdr, mcu_dev_id);
    if (vr != BL_VALIDATE_OK) {
        uint8_t reason = BL_REJECT_BAD_HEADER;
        if (vr == BL_ERR_MCU_ID)       reason = BL_REJECT_BAD_MCU;
        if (vr == BL_ERR_HW_PROFILE)   reason = BL_REJECT_BAD_PROFILE;
        if (vr == BL_ERR_BL_VERSION)   reason = BL_REJECT_BAD_BL_VER;
        if (vr == BL_ERR_APP_SIZE)      reason = BL_REJECT_BAD_SIZE;
        pl[0] = BL_RESP_ERR;
        pl[1] = reason;
        put_u32le(&pl[2], 0u);
        put_u32le(&pl[6], 0u);
        return encode_frame(out, PKT_BOOT_UPDATE_BEGIN, seq, true, pl, 10u);
    }

    /* Accepted — set up update context */
    bl_protocol_reset_ctx();
    s_update_ctx.state        = UPD_RECEIVING;
    s_update_ctx.app_size     = hdr->app_size;
    s_update_ctx.total_chunks = (hdr->app_size + BL_CHUNK_SIZE - 1u) / BL_CHUNK_SIZE;
    s_update_ctx.erased_up_to = APP_START_ADDR;  /* erase lazily as chunks arrive */

    pl[0] = BL_RESP_OK;
    pl[1] = 0u;  /* no reject reason */
    put_u32le(&pl[2], BL_CHUNK_SIZE);
    put_u32le(&pl[6], s_update_ctx.total_chunks);
    return encode_frame(out, PKT_BOOT_UPDATE_BEGIN, seq, false, pl, 10u);
}

static uint16_t handle_update_chunk(uint8_t seq,
                                    const uint8_t *payload, uint16_t plen,
                                    uint8_t *out) {
    uint8_t status = BL_RESP_ERR;

    if (s_update_ctx.state != UPD_RECEIVING) { goto done; }
    if (plen < 8u) { goto done; }  /* need chunk_index(4) + chunk_len(4) */

    uint32_t chunk_idx = get_u32le(&payload[0]);
    uint32_t chunk_len = get_u32le(&payload[4]);
    const uint8_t *data = &payload[8];

    if (chunk_idx != s_update_ctx.next_chunk) { goto done; }
    if (chunk_len > BL_CHUNK_SIZE)            { goto done; }
    if ((uint16_t)(8u + chunk_len) > plen)    { goto done; }
    if (s_update_ctx.bytes_written + chunk_len > s_update_ctx.app_size) { goto done; }

    uint32_t flash_addr = APP_START_ADDR + s_update_ctx.bytes_written;

    /* Erase pages lazily up to the end of this chunk */
    uint32_t chunk_end = flash_addr + chunk_len - 1u;
    if (ensure_erased_through(chunk_end) != BL_FLASH_OK) { goto done; }

    /* Write halfwords; pad odd last byte with 0xFF */
    uint32_t i = 0u;
    for (; i + 1u < chunk_len; i += 2u) {
        uint16_t hw = (uint16_t)data[i] | ((uint16_t)data[i + 1u] << 8u);
        if (bl_flash_write_halfword(flash_addr + i, hw) != BL_FLASH_OK) { goto done; }
    }
    if (i < chunk_len) {  /* one byte remaining */
        uint16_t hw = (uint16_t)data[i] | 0xFF00u;
        if (bl_flash_write_halfword(flash_addr + i, hw) != BL_FLASH_OK) { goto done; }
    }

    s_update_ctx.bytes_written += chunk_len;
    s_update_ctx.next_chunk++;
    status = BL_RESP_OK;

done:
    return encode_frame(out, PKT_BOOT_UPDATE_CHUNK, seq, (status != BL_RESP_OK),
                        &status, 1u);
}

static uint16_t handle_update_finalize(uint8_t seq, uint8_t *out) {
    uint8_t pl[5];
    pl[0] = BL_RESP_ERR;
    put_u32le(&pl[1], 0u);

    if (s_update_ctx.state != UPD_RECEIVING) { goto done; }
    if (s_update_ctx.next_chunk != s_update_ctx.total_chunks) { goto done; }

    uint32_t computed = bl_flash_crc32_region(APP_START_ADDR, s_update_ctx.app_size);
    put_u32le(&pl[1], computed);
    pl[0] = BL_RESP_OK;
    bl_protocol_reset_ctx();

done:
    return encode_frame(out, PKT_BOOT_UPDATE_FINALIZE, seq,
                        (pl[0] != BL_RESP_OK), pl, 5u);
}

static uint16_t handle_update_abort(uint8_t seq, uint8_t *out) {
    bl_protocol_reset_ctx();
    uint8_t status = BL_RESP_OK;
    return encode_frame(out, PKT_BOOT_UPDATE_ABORT, seq, false, &status, 1u);
}

/* ── Frame decoder + dispatcher ──────────────────────────────────────────── */

/* Decode one complete wire frame sitting in buf[0..frame_len-1].
 * Fills *out_len with the response frame length.
 * Returns false on SOF or CRC error (no response). */
static bool dispatch(const uint8_t *buf, uint16_t frame_len,
                     uint8_t *out, uint16_t *out_len) {
    if (frame_len < FRAME_OVERHEAD) { return false; }
    if (buf[0] != SOF0 || buf[1] != SOF1) { return false; }

    uint16_t pkt_id  = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8u);
    uint8_t  seq     = buf[5];
    uint16_t plen    = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8u);

    if (frame_len < (uint16_t)(FRAME_OVERHEAD + plen)) { return false; }

    /* Verify CRC */
    uint16_t recv_crc = ((uint16_t)buf[8u + plen] << 8u) | buf[9u + plen];
    uint16_t calc_crc = crc16(buf, (uint16_t)(8u + plen));
    if (recv_crc != calc_crc) { return false; }

    const uint8_t *payload = &buf[8];
    uint16_t rlen = 0u;

    switch (pkt_id) {
    case PKT_GET_CAPABILITIES:
        rlen = handle_get_capabilities(seq, out);
        break;
    case PKT_GET_BOOT_INFO:
        rlen = handle_get_boot_info(seq, out);
        break;
    case PKT_BOOT_UPDATE_BEGIN:
        rlen = handle_update_begin(seq, payload, plen, out);
        break;
    case PKT_BOOT_UPDATE_CHUNK:
        rlen = handle_update_chunk(seq, payload, plen, out);
        break;
    case PKT_BOOT_UPDATE_FINALIZE:
        rlen = handle_update_finalize(seq, out);
        break;
    case PKT_BOOT_UPDATE_ABORT:
        rlen = handle_update_abort(seq, out);
        break;
    default:
        /* Unknown packet — send error frame with empty payload */
        rlen = encode_frame(out, pkt_id, seq, true, NULL, 0u);
        break;
    }

    *out_len = rlen;
    return true;
}

/* ── Host-build test interface ───────────────────────────────────────────── */
#ifdef BMS_HOST_BUILD

bool bl_protocol_process_frame(const uint8_t *frame, uint16_t frame_len,
                               uint8_t *out_buf, uint16_t *out_len) {
    return dispatch(frame, frame_len, out_buf, out_len);
}

#endif /* BMS_HOST_BUILD */

/* ── Hardware UART protocol loop ─────────────────────────────────────────── */
#ifndef BMS_HOST_BUILD

/* RX state machine */
typedef enum {
    RX_SOF0, RX_SOF1,
    RX_ID_LO, RX_ID_HI,
    RX_FLAGS, RX_SEQ,
    RX_LEN_LO, RX_LEN_HI,
    RX_PAYLOAD,
    RX_CRC_HI, RX_CRC_LO,
} RxState;

#define RX_BUF_SIZE  (FRAME_OVERHEAD + BL_MAX_PAYLOAD + 8u)

void bl_protocol_run(void) {
    static uint8_t rx_buf[RX_BUF_SIZE];
    RxState  state     = RX_SOF0;
    uint16_t rx_idx    = 0u;
    uint16_t payload_len = 0u;

    bl_protocol_reset_ctx();

    while (1) {
        if (!bl_uart_rx_ready()) { continue; }
        uint8_t b = bl_uart_read_byte();

        switch (state) {
        case RX_SOF0:
            if (b == SOF0) { rx_buf[0] = b; rx_idx = 1u; state = RX_SOF1; }
            break;
        case RX_SOF1:
            if (b == SOF1) { rx_buf[1] = b; rx_idx = 2u; state = RX_ID_LO; }
            else           { state = (b == SOF0) ? RX_SOF1 : RX_SOF0; }
            break;
        case RX_ID_LO:
            rx_buf[rx_idx++] = b; state = RX_ID_HI;   break;
        case RX_ID_HI:
            rx_buf[rx_idx++] = b; state = RX_FLAGS;    break;
        case RX_FLAGS:
            rx_buf[rx_idx++] = b; state = RX_SEQ;      break;
        case RX_SEQ:
            rx_buf[rx_idx++] = b; state = RX_LEN_LO;   break;
        case RX_LEN_LO:
            rx_buf[rx_idx++] = b;
            payload_len = (uint16_t)b;
            state = RX_LEN_HI;
            break;
        case RX_LEN_HI:
            rx_buf[rx_idx++] = b;
            payload_len |= (uint16_t)((uint16_t)b << 8u);
            if (payload_len > BL_MAX_PAYLOAD) {
                /* Oversize frame — discard */
                state = RX_SOF0; rx_idx = 0u; payload_len = 0u;
            } else if (payload_len == 0u) {
                state = RX_CRC_HI;
            } else {
                state = RX_PAYLOAD;
            }
            break;
        case RX_PAYLOAD:
            rx_buf[rx_idx++] = b;
            if (rx_idx == (uint16_t)(8u + payload_len)) {
                state = RX_CRC_HI;
            }
            break;
        case RX_CRC_HI:
            rx_buf[rx_idx++] = b; state = RX_CRC_LO;   break;
        case RX_CRC_LO:
            rx_buf[rx_idx++] = b;
            {
                uint16_t frame_len = rx_idx;
                uint16_t out_len   = 0u;
                if (dispatch(rx_buf, frame_len, s_resp_buf, &out_len)) {
                    bl_uart_write(s_resp_buf, out_len);
                }
            }
            state = RX_SOF0; rx_idx = 0u; payload_len = 0u;
            break;
        }
    }
}

#endif /* !BMS_HOST_BUILD */
