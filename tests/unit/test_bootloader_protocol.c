/* test_bootloader_protocol.c — Unit tests for bl_protocol.c (BMS_HOST_BUILD).
 *
 * Builds frames by hand using the BMS framing spec:
 *   SOF(AA 55) + PKT_ID(2LE) + FLAGS(1) + SEQ(1) + LEN(2LE) + PAYLOAD + CRC(2BE)
 * CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
 */
#include "unity.h"
#include "bl_protocol.h"
#include "bl_flash.h"
#include "bl_validate.h"
#include "bl_config.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Frame helpers ────────────────────────────────────────────────────────── */

static uint16_t crc16(const uint8_t *d, uint16_t n) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8u);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1u) ^ 0x1021u) : (uint16_t)(crc << 1u);
    }
    return crc;
}

static uint16_t build_frame(uint8_t *out, uint16_t pkt_id, uint8_t seq,
                             const uint8_t *payload, uint16_t plen) {
    out[0] = 0xAAu; out[1] = 0x55u;
    out[2] = (uint8_t)(pkt_id & 0xFFu);
    out[3] = (uint8_t)((pkt_id >> 8u) & 0xFFu);
    out[4] = 0x00u;  /* FLAGS: request */
    out[5] = seq;
    out[6] = (uint8_t)(plen & 0xFFu);
    out[7] = (uint8_t)((plen >> 8u) & 0xFFu);
    if (plen > 0u && payload != NULL) memcpy(&out[8], payload, plen);
    uint16_t c = crc16(out, (uint16_t)(8u + plen));
    out[8u + plen]     = (uint8_t)(c >> 8u);
    out[9u + plen]     = (uint8_t)(c & 0xFFu);
    return (uint16_t)(10u + plen);
}

/* Decode a uint16 LE from response payload */
static uint16_t get_u16(const uint8_t *resp, uint16_t off) {
    return (uint16_t)resp[8u + off] | ((uint16_t)resp[9u + off] << 8u);
}
static uint32_t get_u32(const uint8_t *resp, uint16_t off) {
    return (uint32_t)resp[8u + off]
         | ((uint32_t)resp[9u + off]  <<  8u)
         | ((uint32_t)resp[10u + off] << 16u)
         | ((uint32_t)resp[11u + off] << 24u);
}

static FirmwarePackageHeader make_valid_header(uint32_t app_size) {
    FirmwarePackageHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pkg_magic            = PKG_MAGIC;
    hdr.pkg_version          = 1u;
    hdr.hw_profile_id        = HW_PROFILE_ID;
    hdr.target_mcu_id        = STM32F303VC_DEV_ID;
    hdr.image_type           = 0x01u;
    hdr.app_start_addr       = APP_START_ADDR;
    hdr.app_size             = app_size;
    hdr.app_crc32            = 0xDEADBEEFu;  /* placeholder */
    hdr.fw_version[0]        = 1u;
    hdr.min_bootloader_version[0] = 0u;
    hdr.required_config_schema    = 1u;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    return hdr;
}

/* ── Test state ───────────────────────────────────────────────────────────── */

#define OUTBUF_SIZE  (10u + 512u + 16u)
static uint8_t s_req[OUTBUF_SIZE];
static uint8_t s_resp[OUTBUF_SIZE];

void setUp(void) {
    bl_protocol_reset_ctx();
    bl_flash_sim_reset();
}

void tearDown(void) {}

/* ── Tests: GET_CAPABILITIES ─────────────────────────────────────────────── */

void test_get_capabilities_firmware_type(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 1u, NULL, 0u);
    uint16_t out_len = 0u;
    TEST_ASSERT_TRUE(bl_protocol_process_frame(s_req, rlen, s_resp, &out_len));
    /* FLAGS bit0 must be set (IS_RESPONSE) */
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[4] & 0x01u);
    /* firmware_type at payload[0..1] = FIRMWARE_TYPE_BOOTLOADER = 0x0002 */
    TEST_ASSERT_EQUAL_UINT16(0x0002u, get_u16(s_resp, 0u));
}

void test_get_capabilities_hw_profile(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 0u, NULL, 0u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    /* hw_profile_id at payload[5..6] */
    TEST_ASSERT_EQUAL_UINT16(HW_PROFILE_ID, get_u16(s_resp, 5u));
}

void test_get_capabilities_protocol_version(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 0u, NULL, 0u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT16(BL_PROTOCOL_VERSION, get_u16(s_resp, 7u));
}

void test_get_capabilities_flash_app_size(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 0u, NULL, 0u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT32(APP_REGION_SIZE, get_u32(s_resp, 18u));
}

void test_get_capabilities_response_crc_valid(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 3u, NULL, 0u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    uint16_t plen = get_u16(s_resp, /* byte 6/7 as LE */ 0u);
    /* plen is embedded in frame header at byte 6 */
    plen = (uint16_t)s_resp[6] | ((uint16_t)s_resp[7] << 8u);
    uint16_t expected_crc = crc16(s_resp, (uint16_t)(8u + plen));
    uint16_t recv_crc = ((uint16_t)s_resp[8u + plen] << 8u) | s_resp[9u + plen];
    TEST_ASSERT_EQUAL_UINT16(expected_crc, recv_crc);
}

/* ── Tests: GET_BOOT_INFO ─────────────────────────────────────────────────── */

void test_get_boot_info_returns_response(void) {
    uint16_t rlen = build_frame(s_req, 0x0401u, 0u, NULL, 0u);
    uint16_t out_len = 0u;
    TEST_ASSERT_TRUE(bl_protocol_process_frame(s_req, rlen, s_resp, &out_len));
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[4] & 0x01u);  /* IS_RESPONSE */
    TEST_ASSERT_GREATER_THAN(10u, out_len);
}

/* ── Tests: bad frame ─────────────────────────────────────────────────────── */

void test_bad_sof_rejected(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 0u, NULL, 0u);
    s_req[0] = 0xBBu;  /* corrupt SOF */
    uint16_t out_len = 0u;
    TEST_ASSERT_FALSE(bl_protocol_process_frame(s_req, rlen, s_resp, &out_len));
}

void test_bad_crc_rejected(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 0u, NULL, 0u);
    s_req[rlen - 1u] ^= 0xFFu;  /* flip last CRC byte */
    uint16_t out_len = 0u;
    TEST_ASSERT_FALSE(bl_protocol_process_frame(s_req, rlen, s_resp, &out_len));
}

void test_frame_too_short_rejected(void) {
    uint16_t out_len = 0u;
    uint8_t tiny[4] = {0xAA, 0x55, 0x01, 0x00};
    TEST_ASSERT_FALSE(bl_protocol_process_frame(tiny, 4u, s_resp, &out_len));
}

void test_unknown_pkt_id_returns_error(void) {
    uint16_t rlen = build_frame(s_req, 0xFFFFu, 7u, NULL, 0u);
    uint16_t out_len = 0u;
    TEST_ASSERT_TRUE(bl_protocol_process_frame(s_req, rlen, s_resp, &out_len));
    /* IS_ERROR bit must be set */
    TEST_ASSERT_EQUAL_UINT8(0x02u, s_resp[4] & 0x02u);
}

/* ── Tests: BOOT_UPDATE_BEGIN ─────────────────────────────────────────────── */

void test_begin_with_valid_header_accepted(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    TEST_ASSERT_TRUE(bl_protocol_process_frame(s_req, rlen, s_resp, &out_len));
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);   /* result: BL_RESP_OK */
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[9]);   /* no reject reason */
}

void test_begin_chunk_size_correct(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT32(256u, get_u32(s_resp, 2u));  /* expected_chunk_size */
}

void test_begin_total_chunks_correct(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    /* 512 bytes / 256 per chunk = 2 chunks */
    TEST_ASSERT_EQUAL_UINT32(2u, get_u32(s_resp, 6u));
}

void test_begin_with_bad_magic_rejected(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    hdr.pkg_magic = 0xDEADBEEFu;
    hdr.pkg_header_crc32 = bl_crc32((const uint8_t *)&hdr, 0x26u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[8]);  /* BL_RESP_ERR */
}

void test_begin_with_short_payload_rejected(void) {
    uint8_t tiny_payload[10] = {0};
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u, tiny_payload, 10u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[8]);  /* BL_RESP_ERR */
}

/* ── Tests: BOOT_UPDATE_CHUNK ─────────────────────────────────────────────── */

static uint8_t s_chunk_req[10u + 8u + 256u];

static uint16_t build_chunk_frame(uint8_t *out, uint32_t chunk_idx,
                                   const uint8_t *data, uint32_t dlen) {
    uint8_t payload[8u + 256u];
    payload[0] = (uint8_t)(chunk_idx & 0xFFu);
    payload[1] = (uint8_t)((chunk_idx >> 8u) & 0xFFu);
    payload[2] = (uint8_t)((chunk_idx >> 16u) & 0xFFu);
    payload[3] = (uint8_t)((chunk_idx >> 24u) & 0xFFu);
    payload[4] = (uint8_t)(dlen & 0xFFu);
    payload[5] = (uint8_t)((dlen >> 8u) & 0xFFu);
    payload[6] = (uint8_t)((dlen >> 16u) & 0xFFu);
    payload[7] = (uint8_t)((dlen >> 24u) & 0xFFu);
    memcpy(&payload[8], data, dlen);
    return build_frame(out, 0x0404u, 0u, payload, (uint16_t)(8u + dlen));
}

void test_chunk_before_begin_rejected(void) {
    uint8_t data[256];
    memset(data, 0xABu, sizeof(data));
    uint16_t rlen = build_chunk_frame(s_chunk_req, 0u, data, 256u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[8]);  /* BL_RESP_ERR */
}

void test_chunk_wrong_index_rejected(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);  /* BEGIN */

    uint8_t data[256];
    memset(data, 0x11u, sizeof(data));
    rlen = build_chunk_frame(s_chunk_req, 1u, data, 256u);  /* skip chunk 0 */
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[8]);  /* BL_RESP_ERR */
}

void test_chunk_accepted_in_order(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);  /* BEGIN */

    uint8_t data[256];
    memset(data, 0xAAu, sizeof(data));
    rlen = build_chunk_frame(s_chunk_req, 0u, data, 256u);
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);  /* BL_RESP_OK */
}

/* ── Tests: BOOT_UPDATE_FINALIZE ─────────────────────────────────────────── */

void test_finalize_before_all_chunks_rejected(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);  /* BEGIN */

    /* Send only first chunk of two */
    uint8_t data[256];
    memset(data, 0xBBu, sizeof(data));
    rlen = build_chunk_frame(s_chunk_req, 0u, data, 256u);
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);

    /* FINALIZE without second chunk */
    rlen = build_frame(s_req, 0x0405u, 0u, NULL, 0u);
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[8]);  /* BL_RESP_ERR */
}

void test_full_update_flow_crc_matches(void) {
    /* Use a 512-byte image (2 chunks) of known data */
    static const uint32_t APP_SIZE = 512u;

    FirmwarePackageHeader hdr = make_valid_header(APP_SIZE);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);  /* BEGIN accepted */

    uint8_t image[512];
    for (uint32_t i = 0u; i < APP_SIZE; i++) image[i] = (uint8_t)(i & 0xFFu);

    /* Chunk 0 */
    rlen = build_chunk_frame(s_chunk_req, 0u, &image[0], 256u);
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);

    /* Chunk 1 */
    rlen = build_chunk_frame(s_chunk_req, 1u, &image[256], 256u);
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);

    /* FINALIZE */
    rlen = build_frame(s_req, 0x0405u, 0u, NULL, 0u);
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);  /* BL_RESP_OK */

    /* Computed CRC should match CRC-32 of the image data */
    uint32_t expected_crc = bl_crc32(image, APP_SIZE);
    uint32_t got_crc = (uint32_t)s_resp[9]
                     | ((uint32_t)s_resp[10] <<  8u)
                     | ((uint32_t)s_resp[11] << 16u)
                     | ((uint32_t)s_resp[12] << 24u);
    TEST_ASSERT_EQUAL_UINT32(expected_crc, got_crc);
}

/* ── Tests: BOOT_UPDATE_ABORT ─────────────────────────────────────────────── */

void test_abort_resets_state(void) {
    FirmwarePackageHeader hdr = make_valid_header(512u);
    uint16_t rlen = build_frame(s_req, 0x0403u, 0u,
                                (const uint8_t *)&hdr, (uint16_t)sizeof(hdr));
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);  /* BEGIN */

    /* ABORT */
    rlen = build_frame(s_req, 0x0406u, 0u, NULL, 0u);
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);  /* BL_RESP_OK */

    /* CHUNK after ABORT should fail */
    uint8_t data[256];
    memset(data, 0u, sizeof(data));
    rlen = build_chunk_frame(s_chunk_req, 0u, data, 256u);
    bl_protocol_process_frame(s_chunk_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, s_resp[8]);  /* BL_RESP_ERR */
}

void test_abort_before_begin_still_ok(void) {
    uint16_t rlen = build_frame(s_req, 0x0406u, 0u, NULL, 0u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(0x00u, s_resp[8]);  /* BL_RESP_OK */
}

/* ── Sequence number echoed ───────────────────────────────────────────────── */

void test_seq_echoed_in_response(void) {
    uint16_t rlen = build_frame(s_req, 0x0001u, 42u, NULL, 0u);
    uint16_t out_len = 0u;
    bl_protocol_process_frame(s_req, rlen, s_resp, &out_len);
    TEST_ASSERT_EQUAL_UINT8(42u, s_resp[5]);
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_get_capabilities_firmware_type);
    RUN_TEST(test_get_capabilities_hw_profile);
    RUN_TEST(test_get_capabilities_protocol_version);
    RUN_TEST(test_get_capabilities_flash_app_size);
    RUN_TEST(test_get_capabilities_response_crc_valid);

    RUN_TEST(test_get_boot_info_returns_response);

    RUN_TEST(test_bad_sof_rejected);
    RUN_TEST(test_bad_crc_rejected);
    RUN_TEST(test_frame_too_short_rejected);
    RUN_TEST(test_unknown_pkt_id_returns_error);

    RUN_TEST(test_begin_with_valid_header_accepted);
    RUN_TEST(test_begin_chunk_size_correct);
    RUN_TEST(test_begin_total_chunks_correct);
    RUN_TEST(test_begin_with_bad_magic_rejected);
    RUN_TEST(test_begin_with_short_payload_rejected);

    RUN_TEST(test_chunk_before_begin_rejected);
    RUN_TEST(test_chunk_wrong_index_rejected);
    RUN_TEST(test_chunk_accepted_in_order);

    RUN_TEST(test_finalize_before_all_chunks_rejected);
    RUN_TEST(test_full_update_flow_crc_matches);

    RUN_TEST(test_abort_resets_state);
    RUN_TEST(test_abort_before_begin_still_ok);

    RUN_TEST(test_seq_echoed_in_response);

    return UNITY_END();
}
