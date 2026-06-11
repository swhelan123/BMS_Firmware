/* bms_state.c — BMS state machine.
 *
 * Transition summary:
 *   INIT      → STANDBY  : immediately on first tick
 *   STANDBY   → DISCHARGE: discharge requested, no discharge-blocking faults
 *   STANDBY   → CHARGE   : charger present, no charge-blocking faults
 *   DISCHARGE → STANDBY  : discharge released or discharge-blocking fault
 *   CHARGE    → STANDBY  : charger disconnected or charge-blocking fault
 *   any       → FAULT    : fatal fault active
 *   FAULT     → STANDBY  : all faults (active and latched) clear
 *
 * MASTER_OK semantics (per docs/01_hardware_contract.md §11): "BMS is healthy
 * and operating; system may proceed". It is requested in every operational
 * state (STANDBY/DISCHARGE/CHARGE) and gated by FAULT_BLOCKS_MASTER_OK_MASK
 * in bms_outputs — so the shutdown-circuit branch closes whenever the BMS is
 * healthy, and opens on any blocking fault.
 */
#include "bms_state.h"
#include "bms_faults.h"
#include "bms_outputs.h"
#include "bms_hal.h"
#include "bms_constants.h"
#include <string.h>

static BmsState s_state;
static bool     s_charger_present;
static bool     s_discharge_requested;
static bool     s_bl_entry_requested;

void bms_state_init(void) {
    s_state               = BMS_STATE_INIT;
    s_charger_present     = false;
    s_discharge_requested = false;
    s_bl_entry_requested  = false;
}

void bms_state_tick(const CellSnapshot    *cells,
                     const TempSnapshot    *temps,
                     const PackMeasurement *pack,
                     uint64_t               active_faults,
                     BmsPermissionRequest  *req_out) {
    (void)cells; (void)temps; (void)pack;

    memset(req_out, 0, sizeof(*req_out));

    /* Bootloader entry takes unconditional priority. */
    if (s_bl_entry_requested) {
        bms_outputs_deassert_all();
        RTC->BKP0R = BL_ENTRY_FLAG;
        SCB->AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESETREQ;
        while (1) { /* wait for reset */ }
    }

    /* Fatal fault forces FAULT state and drops all permissions. */
    if (active_faults & FAULT_FATAL_MASK) {
        s_state = BMS_STATE_FAULT;
        return;
    }

    switch (s_state) {
        case BMS_STATE_INIT:
            s_state = BMS_STATE_STANDBY;
            break;

        case BMS_STATE_STANDBY:
            /* Healthy and idle: MasterOk asserted (health signal). */
            req_out->want_master_ok = true;

            if (s_discharge_requested &&
                !(active_faults & FAULT_BLOCKS_DISCHARGE_MASK)) {
                s_state = BMS_STATE_DISCHARGE;
            } else if (s_charger_present &&
                       !(active_faults & FAULT_BLOCKS_CHARGE_MASK)) {
                s_state = BMS_STATE_CHARGE;
            }
            break;

        case BMS_STATE_DISCHARGE:
            if (!s_discharge_requested ||
                (active_faults & FAULT_BLOCKS_DISCHARGE_MASK)) {
                s_state = BMS_STATE_STANDBY;
                break;
            }
            req_out->want_master_ok = true;
            req_out->want_discharge = true;
            break;

        case BMS_STATE_CHARGE:
            if (!s_charger_present ||
                (active_faults & FAULT_BLOCKS_CHARGE_MASK)) {
                s_state = BMS_STATE_STANDBY;
                break;
            }
            req_out->want_master_ok      = true;
            req_out->want_charge         = true;
            req_out->want_charger_safety = true;
            break;

        case BMS_STATE_FAULT:
            if (!active_faults) { s_state = BMS_STATE_STANDBY; }
            break;

        case BMS_STATE_SHUTDOWN:
            /* No permissions; wait for power-down. */
            break;
    }
}

BmsState bms_state_get(void) { return s_state; }

void bms_state_request_bootloader_entry(void) {
    s_bl_entry_requested = true;
}

void bms_state_notify_charger_present(bool present) {
    s_charger_present = present;
}

void bms_state_set_discharge_requested(bool requested) {
    s_discharge_requested = requested;
}
