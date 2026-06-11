/* bms_outputs.c — permission gating and output application. */
#include "bms_outputs.h"
#include "board_outputs.h"

void bms_outputs_apply(const BmsPermissionRequest *req,
                        uint64_t active_faults,
                        uint64_t latched_faults) {
    /* Latched faults block permissions exactly like active faults: a fault
     * that latched (e.g. cell OV/UV, overcurrent) keeps its permissions
     * blocked until explicitly cleared, even after the condition resolves. */
    uint64_t blocking = active_faults | latched_faults;

    /* Fatal faults: deassert everything immediately */
    if (blocking & FAULT_FATAL_MASK) {
        board_outputs_disable_all();
        return;
    }

    bool master_ok = req->want_master_ok
                     && !(blocking & FAULT_BLOCKS_MASTER_OK_MASK);

    bool discharge = req->want_discharge
                     && !(blocking & FAULT_BLOCKS_DISCHARGE_MASK);

    bool charge    = req->want_charge
                     && !(blocking & FAULT_BLOCKS_CHARGE_MASK);

    bool charger_safety = req->want_charger_safety
                          && !(blocking & FAULT_BLOCKS_CHARGER_SAFETY_MASK);

    board_outputs_set_master_ok(master_ok);
    board_outputs_set_discharge_permission(discharge);
    board_outputs_set_charge_permission(charge);
    board_outputs_set_charger_safety(charger_safety);
}

void bms_outputs_deassert_all(void) {
    board_outputs_disable_all();
}

BmsOutputsBitmask bms_outputs_get_state(void) {
    return board_outputs_get_state();
}
