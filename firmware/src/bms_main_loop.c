/* bms_main_loop.c — BMS application main loop and subsystem orchestration. */
#include "bms_main_loop.h"
#include "bms_config.h"
#include "bms_measurements.h"
#include "bms_faults.h"
#include "bms_outputs.h"
#include "bms_state.h"
#include "bms_balance.h"
#include "bms_protocol.h"
#include "bms_can.h"
#include "bms_soc.h"
#include "ltc6812.h"
#include "board_outputs.h"
#include "board_clock.h"
#include "bms_hal.h"
#include "bms_constants.h"

static uint32_t s_last_cell_ms;
static uint32_t s_last_temp_ms;
static uint32_t s_last_pack_ms;

#define CELL_CYCLE_PERIOD_MS   100u
#define TEMP_CYCLE_PERIOD_MS   500u
#define PACK_CYCLE_PERIOD_MS   100u

static void kick_iwdg(void) {
    IWDG->KR = IWDG_KR_RELOAD;
}

void bms_main_loop_init(void) {
    /* Load config from flash. If no valid config, load defaults and set fault. */
    BmsResult cfg_r = bms_config_load();

    /* Initialise LTC6812 chains with safe configuration */
    ltc6812_init_chain(BMS_CHAIN_CELL, CELL_IC_COUNT);
    ltc6812_init_chain(BMS_CHAIN_TEMP, TEMP_IC_COUNT);

    /* Initialise BMS subsystems */
    bms_state_init();
    bms_protocol_init();
    bms_can_init();
    bms_soc_init();

    if (cfg_r != BMS_OK) {
        bms_faults_set(FAULT_BIT_CONFIG_INVALID);
    }

    /* Start IWDG: prescaler /32, reload = 1250 → ~500 ms timeout at 40 kHz LSI */
    IWDG->KR  = IWDG_KR_UNLOCK;
    IWDG->PR  = 3u; /* /32 */
    IWDG->RLR = 1250u;
    IWDG->KR  = IWDG_KR_ENABLE;

    s_last_cell_ms = board_clock_get_ms();
    s_last_temp_ms = board_clock_get_ms();
    s_last_pack_ms = board_clock_get_ms();
}

void bms_main_loop_run(void) {
    while (1) {
        uint32_t now = board_clock_get_ms();

        /* ── Kick watchdog ────────────────────────────────────────────────── */
        kick_iwdg();

        /* ── Config pointer (used by measurement and fault paths) ────────── */
        const BmsConfig *cfg = bms_config_get();

        /* ── Measurement cycles ───────────────────────────────────────────── */
        if ((now - s_last_cell_ms) >= CELL_CYCLE_PERIOD_MS) {
            s_last_cell_ms = now;
            BmsResult r = bms_measurements_run_cell_cycle();
            if (r == BMS_ERR_PEC) { bms_faults_report_pec_error(BMS_CHAIN_CELL); }
            bms_soc_maybe_init_from_cells(bms_measurements_get_cells(), cfg->capacity_mah);
        }

        if ((now - s_last_temp_ms) >= TEMP_CYCLE_PERIOD_MS) {
            s_last_temp_ms = now;
            bms_measurements_run_temp_cycle();
            /* S-outputs guaranteed cleared inside run_temp_cycle() */
        }

        if ((now - s_last_pack_ms) >= PACK_CYCLE_PERIOD_MS) {
            s_last_pack_ms = now;
            bms_measurements_run_pack_cycle();
            const PackMeasurement *pack = bms_measurements_get_pack();
            if (pack->i_batt_valid) {
                bms_soc_update(pack->i_batt_ma, PACK_CYCLE_PERIOD_MS, cfg->capacity_mah);
            }
        }

        /* ── Fault evaluation ─────────────────────────────────────────────── */
        bms_faults_evaluate(bms_measurements_get_cells(),
                             bms_measurements_get_temps(),
                             bms_measurements_get_pack(),
                             cfg);

        uint64_t active  = bms_faults_get_active();
        uint64_t latched = bms_faults_get_latched();

        /* ── Fatal fault handling ──────────────────────────────────────────── */
        if (active & FAULT_FATAL_MASK) {
            bms_outputs_deassert_all();
            /* Spin until IWDG resets the system */
            while (1) { /* do not kick watchdog */ }
        }

        /* ── State machine ────────────────────────────────────────────────── */
        BmsPermissionRequest perm_req;
        bms_state_tick(bms_measurements_get_cells(),
                        bms_measurements_get_temps(),
                        bms_measurements_get_pack(),
                        active, &perm_req);

        /* ── Output gating ────────────────────────────────────────────────── */
        bms_outputs_apply(&perm_req, active, latched);

        /* ── Balance ──────────────────────────────────────────────────────── */
        bms_balance_tick(bms_measurements_get_cells(), active, bms_state_get(), cfg);

        /* ── Protocol ─────────────────────────────────────────────────────── */
        bms_protocol_tick();

        /* ── CAN telemetry ────────────────────────────────────────────────── */
        bms_can_tick();
    }
}
