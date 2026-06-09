/* bms_soc.h — State-of-Charge tracking.
 *
 * Algorithm: OCV lookup for initialization on first valid cell reading,
 * then coulomb counting (integrating pack current) for ongoing tracking.
 *
 * soc_pct_x10 range: 0–1000 (0.0%–100.0%), -1 = not yet initialized.
 */
#pragma once
#include <stdint.h>
#include "bms_types.h"

/* Reset SOC state to unknown. Call once in bms_main_loop_init(). */
void bms_soc_init(void);

/* Initialize SOC from cell OCV if not already done and cells are valid.
 * No-op once initialized. Call after every cell measurement cycle. */
void bms_soc_maybe_init_from_cells(const CellSnapshot *cells, uint32_t capacity_mah);

/* Integrate current into SOC estimate. Call after every successful pack cycle.
 * Positive i_batt_ma = discharge; dt_ms = elapsed time since last call. */
void bms_soc_update(int32_t i_batt_ma, uint32_t dt_ms, uint32_t capacity_mah);

/* Current SOC×10 (0–1000), or -1 if not yet initialized. */
int16_t bms_soc_get_pct_x10(void);
