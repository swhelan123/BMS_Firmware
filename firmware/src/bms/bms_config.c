/* bms_config.c — config load, validate, store, and CRC. */
#include "bms_config.h"
#include "board_flash.h"
#include <string.h>

/* ── Safe defaults ────────────────────────────────────────────────────────── */
static const BmsConfig k_defaults = {
    .magic                       = CONFIG_MAGIC,
    .schema_version              = CONFIG_SCHEMA_VERSION,
    .total_length                = CONFIG_SCHEMA_SIZE,
    .hw_profile_id               = HW_PROFILE_ID,
    .config_generation           = 1u,
    .config_crc32                = 0u, /* filled by bms_config_compute_crc() */
    .reserved_header             = {0},

    .cell_count                  = 75u,
    .temp_count                  = 75u,
    .reserved_topology           = 0u,

    .cell_uv_hard_mv             = 2750u,
    .cell_uv_soft_mv             = 3000u,
    .cell_ov_soft_mv             = 4150u,
    .cell_ov_hard_mv             = 4200u,
    .cell_balance_target_mv      = 3800u,
    .cell_balance_hysteresis_mv  = 10u,
    .cell_nominal_mv             = 3700u,
    .reserved_cell_thresholds    = 0u,

    .temp_charge_warn_cx10       = 400,
    .temp_charge_hard_cx10       = 450,
    .temp_discharge_warn_cx10    = 550,
    .temp_discharge_hard_cx10    = 600,
    .temp_hard_abs_cx10          = 700,
    .temp_cold_charge_limit_cx10 = 0,
    .temp_cold_discharge_limit_cx10 = -200,
    .reserved_temp_thresholds    = 0u,

    .overcurrent_hard_ma         = 100000u,
    .overcurrent_warn_ma         = 80000u,

    .balance_on_time_ms          = 5000u,
    .balance_off_time_ms         = 1000u,

    .temp_settle_time_ms         = 5u,
    .reserved_temp_params        = 0u,

    .stale_data_timeout_ms       = 500u,

    /* All masks: all 75 bits set; top byte 0x07 (bits 75-79 = 0, cells 72/73/74 only) */
    .required_cell_mask          = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x07},
    .required_temp_mask          = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x07},
    .balance_allowed_mask        = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x07},

    /* Theoretical pre-calibration values — refine with hardware measurement.
     * vpack: 4×470kΩ÷1kΩ → AMC1301(×8.2) → OPA2197(×5.893) → 33k/43k → ADC
     * vbat:  R43/R44 (3.3k/3.3k ÷2) → ISL28022 Vbus
     * current: 0.1mΩ shunt → AMC1302(×40) → ÷7.6 → ISL28022 Vin; gain ~1,855,000 */
    .vpack_gain_x1000            = 50706u,
    .vpack_offset_mv             = 0,
    .vbat_gain_x1000             = 2000u,
    .vbat_offset_mv              = 0,
    .current_gain_x1000          = 1000u,   /* placeholder — calibrate on hardware */
    .current_offset_ma           = 0,

    .can_watchdog_timeout_ms     = 0u, /* disabled */
    .can_base_id                 = 0x0500u,
    .reserved_can                = 0u,

    .capacity_mah                = 100000u, /* 100 Ah default — adjust per pack */

    /* 311.0V setpoint, 315.0V clamp (== cell_ov_hard_mv(4200) * cell_count(75)
     * / 100 at default topology), 10.0A CC, 1.0A taper held 5s to terminate. */
    .charge_voltage_setpoint_dv  = 3110u,
    .charge_voltage_max_dv       = 3150u,
    .charge_current_setpoint_da  = 100u,
    .charge_taper_current_da     = 10u,
    .charge_taper_hold_ms        = 5000u,

    .reserved                    = {0},
};

/* ── Active config RAM slot ───────────────────────────────────────────────── */
static BmsConfig s_active_config;
static bool      s_config_loaded;

/* ── CRC-32/ISO-HDLC ─────────────────────────────────────────────────────── */
/* Poly 0xEDB88320 (reversed), init 0xFFFFFFFF, final XOR 0xFFFFFFFF. */
uint32_t bms_config_compute_crc(const BmsConfig *cfg) {
    /* Copy to a temporary buffer with the CRC field zeroed */
    uint8_t buf[CONFIG_SCHEMA_SIZE];
    memcpy(buf, cfg, CONFIG_SCHEMA_SIZE);
    /* CRC field is at offset 14, size 4 */
    buf[14] = 0; buf[15] = 0; buf[16] = 0; buf[17] = 0;

    uint32_t crc = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < CONFIG_SCHEMA_SIZE; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ── Validation ───────────────────────────────────────────────────────────── */
BmsResult bms_config_validate(const BmsConfig *cfg, uint16_t *err_field_offset) {
#define FAIL(off) do { if (err_field_offset) { *err_field_offset = (off); } \
                       return BMS_ERR_CONFIG_INVALID; } while (0)

    if (cfg->magic          != CONFIG_MAGIC)          { FAIL(0); }
    if (cfg->schema_version != CONFIG_SCHEMA_VERSION) { FAIL(4); }
    if (cfg->total_length   != CONFIG_SCHEMA_SIZE)    { FAIL(6); }
    if (cfg->hw_profile_id  != HW_PROFILE_ID)         { FAIL(8); }
    if (cfg->config_generation == CONFIG_INVALID_GENERATION) { FAIL(10); }

    /* CRC check */
    if (cfg->config_crc32 != bms_config_compute_crc(cfg)) { FAIL(14); }

    /* Reserved header must be zero */
    for (int i = 0; i < 46; i++) {
        if (cfg->reserved_header[i] != 0u) { FAIL(18); }
    }

    /* Topology — the pack is built from whole segments (one LTC6812 each),
     * so the active count must be a whole number of segments, at least one
     * and no more than the image's max chain. Cells and temps live on the
     * same slave board, so their counts must match. */
    if (cfg->cell_count == 0u ||
        cfg->cell_count > TOTAL_CELL_COUNT ||
        (cfg->cell_count % CELLS_PER_IC) != 0u) { FAIL(64); }
    if (cfg->temp_count != cfg->cell_count)     { FAIL(65); }
    if (cfg->reserved_topology != 0u)           { FAIL(66); }

    /* INV-01: cell threshold ordering */
    if (cfg->cell_uv_hard_mv >= cfg->cell_uv_soft_mv)          { FAIL(68); }
    if (cfg->cell_uv_soft_mv >= cfg->cell_balance_target_mv)    { FAIL(70); }
    if (cfg->cell_balance_target_mv >= cfg->cell_ov_soft_mv)    { FAIL(76); }
    if (cfg->cell_ov_soft_mv >= cfg->cell_ov_hard_mv)           { FAIL(72); }

    /* Cell balance hysteresis sanity */
    if (cfg->cell_balance_hysteresis_mv >=
        (cfg->cell_ov_soft_mv - cfg->cell_balance_target_mv))   { FAIL(78); }

    /* INV-02 */
    if (cfg->temp_charge_warn_cx10 >= cfg->temp_charge_hard_cx10)   { FAIL(84); }
    if (cfg->temp_charge_hard_cx10 > cfg->temp_hard_abs_cx10)        { FAIL(86); }

    /* INV-03 */
    if (cfg->temp_discharge_warn_cx10 >= cfg->temp_discharge_hard_cx10) { FAIL(88); }
    if (cfg->temp_discharge_hard_cx10 > cfg->temp_hard_abs_cx10)         { FAIL(90); }

    /* INV-04 */
    if (cfg->temp_cold_discharge_limit_cx10 > cfg->temp_cold_charge_limit_cx10) { FAIL(96); }

    /* INV-05 */
    if (cfg->overcurrent_warn_ma > cfg->overcurrent_hard_ma) { FAIL(104); }
    if (cfg->overcurrent_hard_ma == 0u) { FAIL(100); }

    /* Balancing */
    if (cfg->balance_on_time_ms  == 0u) { FAIL(108); }
    if (cfg->balance_off_time_ms == 0u) { FAIL(112); }

    /* Temp settle */
    if (cfg->temp_settle_time_ms == 0u) { FAIL(116); }

    /* Stale timeout */
    if (cfg->stale_data_timeout_ms < 100u) { FAIL(120); }

    /* INV-06: mask reserved bits must be zero */
    if (cfg->required_cell_mask[9]   & CONFIG_MASK_RESERVED_MASK) { FAIL(124); }
    if (cfg->required_temp_mask[9]   & CONFIG_MASK_RESERVED_MASK) { FAIL(134); }
    if (cfg->balance_allowed_mask[9] & CONFIG_MASK_RESERVED_MASK) { FAIL(144); }

    /* INV-07: no mask may reference a channel beyond the active count. A
     * cell/temp on an absent segment can never report a valid reading, so
     * requiring it (or allowing balance on it) would guarantee a fault or
     * drive a non-existent balancer. Bits [cell_count..79] must be zero. */
    for (uint16_t i = cfg->cell_count; i < 80u; i++) {
        uint8_t byte = (uint8_t)(i >> 3u);
        uint8_t bit  = (uint8_t)(i & 7u);
        if (cfg->required_cell_mask[byte]   & (1u << bit)) { FAIL(124); }
        if (cfg->balance_allowed_mask[byte] & (1u << bit)) { FAIL(144); }
    }
    for (uint16_t i = cfg->temp_count; i < 80u; i++) {
        uint8_t byte = (uint8_t)(i >> 3u);
        uint8_t bit  = (uint8_t)(i & 7u);
        if (cfg->required_temp_mask[byte] & (1u << bit)) { FAIL(134); }
    }

    /* Calibration bounds */
    if (cfg->vpack_gain_x1000 == 0u)    { FAIL(154); }
    if (cfg->vbat_gain_x1000 == 0u)     { FAIL(162); }
    if (cfg->current_gain_x1000 == 0u)  { FAIL(166); }

    /* CAN */
    if (cfg->can_base_id > 0x7FFu) { FAIL(176); }

    /* Capacity */
    if (cfg->capacity_mah == 0u) { FAIL(180); }

    /* Charger CAN setpoints (INV-08). The voltage ceiling is tied to the
     * configured topology + cell OV hard limit, not a hardcoded number, so
     * it's automatically correct for both 60- and 75-cell builds. */
    if (cfg->charge_voltage_setpoint_dv == 0u)                              { FAIL(184); }
    if (cfg->charge_voltage_setpoint_dv > cfg->charge_voltage_max_dv)        { FAIL(184); }
    {
        uint32_t max_allowed_dv = ((uint32_t)cfg->cell_ov_hard_mv * cfg->cell_count) / 100u;
        if (cfg->charge_voltage_max_dv > max_allowed_dv)                    { FAIL(186); }
    }
    if (cfg->charge_current_setpoint_da == 0u)                              { FAIL(188); }
    if ((uint32_t)cfg->charge_current_setpoint_da * 100u > cfg->overcurrent_hard_ma) { FAIL(188); }
    if (cfg->charge_taper_current_da == 0u ||
        cfg->charge_taper_current_da >= cfg->charge_current_setpoint_da)   { FAIL(190); }
    if (cfg->charge_taper_hold_ms < 1000u)                                  { FAIL(192); }

    /* Reserved end must be zero */
    for (int i = 0; i < 32; i++) {
        if (cfg->reserved[i] != 0u) { FAIL(194); }
    }

    if (err_field_offset) { *err_field_offset = 0xFFFFu; }
    return BMS_OK;
#undef FAIL
}

/* ── Load from flash ─────────────────────────────────────────────────────── */
BmsResult bms_config_load(void) {
    BmsConfig slot_a, slot_b;
    uint16_t err;
    bool a_ok, b_ok;

    board_flash_read(CONFIG_A_START_ADDR, (uint8_t *)&slot_a, CONFIG_SCHEMA_SIZE);
    board_flash_read(CONFIG_B_START_ADDR, (uint8_t *)&slot_b, CONFIG_SCHEMA_SIZE);

    a_ok = (bms_config_validate(&slot_a, &err) == BMS_OK);
    b_ok = (bms_config_validate(&slot_b, &err) == BMS_OK);

    if (a_ok && b_ok) {
        /* Select higher generation (wraps at 0xFFFFFFFE) */
        BmsConfig *winner = (slot_a.config_generation >= slot_b.config_generation)
                            ? &slot_a : &slot_b;
        memcpy(&s_active_config, winner, CONFIG_SCHEMA_SIZE);
    } else if (a_ok) {
        memcpy(&s_active_config, &slot_a, CONFIG_SCHEMA_SIZE);
    } else if (b_ok) {
        memcpy(&s_active_config, &slot_b, CONFIG_SCHEMA_SIZE);
    } else {
        /* No valid config in flash — load safe defaults */
        bms_config_load_defaults(&s_active_config);
        s_config_loaded = true;
        return BMS_ERR_CONFIG_INVALID; /* caller should set FAULT_CONFIG_INVALID */
    }

    s_config_loaded = true;
    return BMS_OK;
}

const BmsConfig *bms_config_get(void) {
    return &s_active_config;
}

uint8_t bms_config_active_cell_ics(void) {
    /* cell_count is validated as a non-zero multiple of CELLS_PER_IC no
     * greater than TOTAL_CELL_COUNT, so this is always in
     * [MIN_CELL_IC_COUNT, CELL_IC_COUNT]. Clamp defensively anyway — this
     * bounds an isoSPI chain read, and an out-of-range value would walk
     * the static buffers off the end. */
    uint8_t ics = (uint8_t)(s_active_config.cell_count / CELLS_PER_IC);
    if (ics < MIN_CELL_IC_COUNT) { ics = MIN_CELL_IC_COUNT; }
    if (ics > CELL_IC_COUNT)     { ics = CELL_IC_COUNT; }
    return ics;
}

uint8_t bms_config_active_temp_ics(void) {
    uint8_t ics = (uint8_t)(s_active_config.temp_count / TEMPS_PER_IC);
    if (ics < MIN_CELL_IC_COUNT) { ics = MIN_CELL_IC_COUNT; }
    if (ics > TEMP_IC_COUNT)     { ics = TEMP_IC_COUNT; }
    return ics;
}

BmsResult bms_config_apply_ram(const BmsConfig *cfg) {
    uint16_t err;
    BmsResult r = bms_config_validate(cfg, &err);
    if (r != BMS_OK) { return r; }
    memcpy(&s_active_config, cfg, CONFIG_SCHEMA_SIZE);
    return BMS_OK;
}

BmsResult bms_config_store(const BmsConfig *cfg) {
    /* Validate before any flash access */
    uint16_t err;
    BmsResult r = bms_config_validate(cfg, &err);
    if (r != BMS_OK) { return r; }

    /* Determine which slot to overwrite (lower generation, or A if equal) */
    BmsConfig cur_a, cur_b;
    board_flash_read(CONFIG_A_START_ADDR, (uint8_t *)&cur_a, CONFIG_SCHEMA_SIZE);
    board_flash_read(CONFIG_B_START_ADDR, (uint8_t *)&cur_b, CONFIG_SCHEMA_SIZE);
    uint16_t err_a, err_b;
    bool a_ok = (bms_config_validate(&cur_a, &err_a) == BMS_OK);
    bool b_ok = (bms_config_validate(&cur_b, &err_b) == BMS_OK);

    uint32_t target_addr;
    uint32_t new_gen = 1u;
    if (a_ok && b_ok) {
        if (cur_a.config_generation <= cur_b.config_generation) {
            target_addr = CONFIG_A_START_ADDR;
            new_gen = cur_b.config_generation + 1u;
        } else {
            target_addr = CONFIG_B_START_ADDR;
            new_gen = cur_a.config_generation + 1u;
        }
    } else if (!a_ok) {
        target_addr = CONFIG_A_START_ADDR;
        new_gen = b_ok ? cur_b.config_generation + 1u : 1u;
    } else {
        target_addr = CONFIG_B_START_ADDR;
        new_gen = cur_a.config_generation + 1u;
    }

    /* Build new config blob with updated generation and CRC */
    BmsConfig new_cfg;
    memcpy(&new_cfg, cfg, CONFIG_SCHEMA_SIZE);
    new_cfg.config_generation = new_gen;
    new_cfg.config_crc32 = 0u;
    new_cfg.config_crc32 = bms_config_compute_crc(&new_cfg);

    /* Erase and write */
    r = board_flash_erase_config_slot(target_addr);
    if (r != BMS_OK) { return r; }
    r = board_flash_write(target_addr, (const uint8_t *)&new_cfg, CONFIG_SCHEMA_SIZE);
    if (r != BMS_OK) { return r; }

    /* Verify readback */
    BmsConfig verify;
    board_flash_read(target_addr, (uint8_t *)&verify, CONFIG_SCHEMA_SIZE);
    if (bms_config_validate(&verify, &err) != BMS_OK) { return BMS_ERR_FLASH; }

    /* Apply to RAM and signal soft reset (caller responsible for reset) */
    memcpy(&s_active_config, &new_cfg, CONFIG_SCHEMA_SIZE);
    return BMS_OK;
}

void bms_config_load_defaults(BmsConfig *out) {
    memcpy(out, &k_defaults, CONFIG_SCHEMA_SIZE);
    out->config_crc32 = 0u;
    out->config_crc32 = bms_config_compute_crc(out);
}
