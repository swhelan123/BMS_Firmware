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
#include "bms_diagnostics.h"
#include "ltc6812.h"
#include "board_outputs.h"
#include "board_inputs.h"
#include "board_clock.h"
#include "bms_hal.h"
#include "bms_constants.h"

static uint32_t s_last_cell_ms;
static uint32_t s_last_temp_ms;
static uint32_t s_last_pack_ms;
static uint32_t s_last_openwire_ms;

/* Charge-detect debounce state */
static bool     s_cd_stable;     /* debounced charger-present level */
static bool     s_cd_last_raw;   /* last raw sample */
static uint32_t s_cd_change_ms;  /* time the raw level last changed */

#define CELL_CYCLE_PERIOD_MS   100u
#define TEMP_CYCLE_PERIOD_MS   500u
#define PACK_CYCLE_PERIOD_MS   100u

static void kick_iwdg(void) {
    IWDG->KR = IWDG_KR_RELOAD;
}

/* Sample CHARGE_DETECT, debounce it, and feed the state machine inputs.
 * Vehicle-mode policy: discharge is requested whenever no charger is
 * present — the BMS grants permissions as soon as it is healthy, and the
 * vehicle-side shutdown circuit / precharge sequence controls actual
 * energization. */
static void update_state_inputs(uint32_t now) {
    bool raw = board_inputs_charge_detect();
    if (raw != s_cd_last_raw) {
        s_cd_last_raw  = raw;
        s_cd_change_ms = now;
    } else if (raw != s_cd_stable &&
               (now - s_cd_change_ms) >= CHARGE_DETECT_DEBOUNCE_MS) {
        s_cd_stable = raw;
    }
    bms_state_notify_charger_present(s_cd_stable);
    bms_state_set_discharge_requested(!s_cd_stable);
}

/* Periodic open-wire scan. Runs only in STANDBY and CHARGE: the ADOW
 * sequence perturbs cell readings, and a false trip while driving must be
 * avoided. A detected open wire on a required cell latches
 * FAULT_BIT_CELL_OPENWIRE (blocks all permissions until cleared). */
static void run_periodic_openwire(uint32_t now, const BmsConfig *cfg) {
    if ((now - s_last_openwire_ms) < OPENWIRE_SCAN_PERIOD_MS) { return; }
    s_last_openwire_ms = now;

    BmsState st = bms_state_get();
    if (st != BMS_STATE_STANDBY && st != BMS_STATE_CHARGE) { return; }

    kick_iwdg(); /* the scan blocks for several ms; keep margin */
    bool detected[TOTAL_CELL_COUNT];
    BmsResult r = ltc6812_run_open_wire(BMS_CHAIN_CELL, CELL_IC_COUNT, detected);
    bms_diagnostics_set_open_wire(r == BMS_OK, detected);
    if (r == BMS_OK) {
        bms_faults_apply_openwire(detected, cfg);
    }
    /* Scan failure (PEC/SPI) is already escalated via the ISOSPI fault
     * path on the regular cell cycle; do not double-report here. */
}

void bms_main_loop_init(void) {
    /* Capture reset cause first (clears RCC_CSR flags). An IWDG-caused
     * reset means the previous firmware run hung: latch FAULT_BIT_WATCHDOG
     * so all permissions stay blocked until explicitly cleared. */
    bms_diagnostics_init();
    if (bms_diagnostics_get()->reset_cause & RESET_CAUSE_IWDG) {
        bms_faults_set_latched(FAULT_BIT_WATCHDOG);
    }

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

    uint32_t now = board_clock_get_ms();
    /* Back-date the cycle timers so every measurement cycle runs on the very
     * first loop iteration — fault evaluation never sees pre-measurement
     * data. (Unsigned wrap-around makes the subtraction safe near t=0.) */
    s_last_cell_ms     = now - CELL_CYCLE_PERIOD_MS;
    s_last_temp_ms     = now - TEMP_CYCLE_PERIOD_MS;
    s_last_pack_ms     = now - PACK_CYCLE_PERIOD_MS;
    s_last_openwire_ms = now;

    /* Seed charge-detect debounce from the current raw level so a charger
     * already plugged in at boot is recognised after one debounce window. */
    s_cd_stable    = false;
    s_cd_last_raw  = board_inputs_charge_detect();
    s_cd_change_ms = now;
}

void bms_main_loop_run(void) {
    while (1) {
        uint32_t now = board_clock_get_ms();

        /* ── Kick watchdog ────────────────────────────────────────────────── */
        kick_iwdg();

        /* ── Config pointer (used by measurement and fault paths) ────────── */
        const BmsConfig *cfg = bms_config_get();

        /* ── State machine inputs (charge detect → charger/discharge req) ── */
        update_state_inputs(now);

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
        /* Latched faults gate permissions exactly like active ones: a fault
         * that latched keeps blocking until explicitly cleared via protocol. */
        uint64_t blocking = active | latched;

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
                        blocking, &perm_req);

        /* ── Output gating ────────────────────────────────────────────────── */
        bms_outputs_apply(&perm_req, active, latched);

        /* ── Balance ──────────────────────────────────────────────────────── */
        bms_balance_tick(bms_measurements_get_cells(), blocking, bms_state_get(), cfg);

        /* ── Periodic open-wire scan (STANDBY/CHARGE only) ────────────────── */
        run_periodic_openwire(now, cfg);

        /* ── Protocol ─────────────────────────────────────────────────────── */
        bms_protocol_tick();

        /* ── CAN telemetry ────────────────────────────────────────────────── */
        bms_can_tick();
    }
}
