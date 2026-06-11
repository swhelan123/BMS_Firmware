/* bms_soc.c — State-of-Charge: OCV initialization + coulomb counting.
 *
 * OCV table: NMC Li-ion, 13 breakpoints.
 * Coulomb counting: µAh accumulator; 3,600,000 mA·ms = 1 µAh.
 * Positive current = discharge = remaining capacity decreases.
 */
#include "bms_soc.h"
#include "bms_constants.h"

/* ── OCV → SOC lookup table ───────────────────────────────────────────────── */
/* Sorted mv DESCENDING. Based on NMC Li-ion open-circuit voltage curve. */
typedef struct { uint16_t mv; int16_t soc_x10; } OcvPoint;

static const OcvPoint k_ocv_table[] = {
    { 4200, 1000 },
    { 4100,  900 },
    { 4000,  800 },
    { 3900,  700 },
    { 3800,  600 },
    { 3700,  500 },
    { 3600,  400 },
    { 3500,  300 },
    { 3400,  200 },
    { 3300,  150 },
    { 3200,  100 },
    { 3100,   50 },
    { 3000,    0 },
};
#define OCV_TABLE_COUNT  (sizeof(k_ocv_table) / sizeof(k_ocv_table[0]))

static int16_t ocv_to_soc_x10(uint16_t mv) {
    if (mv >= k_ocv_table[0].mv) return 1000;
    if (mv <= k_ocv_table[OCV_TABLE_COUNT - 1u].mv) return 0;
    for (uint8_t i = 0u; i < OCV_TABLE_COUNT - 1u; i++) {
        if (mv <= k_ocv_table[i].mv && mv >= k_ocv_table[i + 1u].mv) {
            int32_t dv = (int32_t)k_ocv_table[i].mv - (int32_t)k_ocv_table[i + 1u].mv;
            int32_t ds = (int32_t)k_ocv_table[i + 1u].soc_x10 - (int32_t)k_ocv_table[i].soc_x10;
            int32_t offset = (int32_t)k_ocv_table[i].mv - (int32_t)mv;
            return (int16_t)((int32_t)k_ocv_table[i].soc_x10 + (offset * ds) / dv);
        }
    }
    return 0;
}

/* ── State ────────────────────────────────────────────────────────────────── */
static int16_t s_soc_pct_x10   = -1;  /* -1 = unknown */
static int64_t s_remaining_uAh =  0;  /* remaining capacity in µAh */
static int64_t s_capacity_uAh  =  0;  /* full capacity in µAh */
static int64_t s_accum_mAms    =  0;  /* sub-µAh carry: 3,600,000 mA·ms = 1 µAh */

void bms_soc_init(void) {
    s_soc_pct_x10   = -1;
    s_remaining_uAh =  0;
    s_capacity_uAh  =  0;
    s_accum_mAms    =  0;
}

void bms_soc_maybe_init_from_cells(const CellSnapshot *cells, uint32_t capacity_mah) {
    if (s_soc_pct_x10 >= 0)               return;  /* already initialized */
    if (cells->overall != MEAS_VALID)      return;
    if (capacity_mah == 0u)               return;

    /* Weakest VALID cell determines pack SOC */
    uint16_t min_mv = UINT16_MAX;
    for (uint8_t i = 0u; i < TOTAL_CELL_COUNT; i++) {
        if (cells->valid[i] && cells->mv[i] < min_mv) {
            min_mv = cells->mv[i];
        }
    }
    if (min_mv == UINT16_MAX) { return; }  /* no valid cell yet */

    int16_t soc_x10    = ocv_to_soc_x10(min_mv);
    s_capacity_uAh     = (int64_t)capacity_mah * 1000LL;
    s_remaining_uAh    = s_capacity_uAh * soc_x10 / 1000LL;
    s_accum_mAms       = 0;
    s_soc_pct_x10      = soc_x10;
}

void bms_soc_update(int32_t i_batt_ma, uint32_t dt_ms, uint32_t capacity_mah) {
    if (s_soc_pct_x10 < 0)               return;
    if (capacity_mah == 0u || s_capacity_uAh == 0) return;

    /* Accumulate mA·ms, convert whole µAh, carry remainder */
    s_accum_mAms += (int64_t)i_batt_ma * (int64_t)dt_ms;
    int64_t delta_uAh = s_accum_mAms / 3600000LL;
    s_accum_mAms     %= 3600000LL;

    s_remaining_uAh -= delta_uAh;
    if (s_remaining_uAh > s_capacity_uAh) s_remaining_uAh = s_capacity_uAh;
    if (s_remaining_uAh < 0LL)            s_remaining_uAh = 0LL;

    s_soc_pct_x10 = (int16_t)(s_remaining_uAh * 1000LL / s_capacity_uAh);
}

int16_t bms_soc_get_pct_x10(void) {
    return s_soc_pct_x10;
}
