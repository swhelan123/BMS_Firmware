"""validator.py — client-side config validation (mirrors firmware bms_config_validate).

Returns (ok: bool, error_field_offset: int, error_message: str).
"""
from .schema import BmsConfig, CONFIG_MAGIC, SCHEMA_VERSION, CONFIG_SCHEMA_SIZE
from ..protocol.packet_defs import HW_PROFILE_ID
from ..protocol.crc import crc32_iso_hdlc
import struct


def validate_config(cfg: BmsConfig) -> tuple:
    """Returns (ok, err_offset, message)."""
    def fail(offset, msg):
        return False, offset, msg

    if cfg.magic != CONFIG_MAGIC:
        return fail(0, "Wrong magic")
    if cfg.schema_version != SCHEMA_VERSION:
        return fail(4, "Wrong schema_version")
    if cfg.total_length != CONFIG_SCHEMA_SIZE:
        return fail(6, "Wrong total_length")
    if cfg.hw_profile_id != HW_PROFILE_ID:
        return fail(8, f"Wrong hw_profile_id: got 0x{cfg.hw_profile_id:04X}")
    if cfg.config_generation == 0xFFFFFFFF:
        return fail(10, "Invalid config_generation")

    # Verify CRC
    blob = cfg.pack()  # pack() zeroes CRC field during computation
    expected_crc = struct.unpack_from('<I', blob, 14)[0]
    if expected_crc != cfg.config_crc32 and cfg.config_crc32 != 0:
        return fail(14, "CRC mismatch")

    if cfg.reserved_header != bytes(46):
        return fail(18, "reserved_header must be zero")
    if cfg.cell_count != 75:
        return fail(64, "cell_count must be 75")
    if cfg.temp_count != 75:
        return fail(65, "temp_count must be 75")
    if cfg.reserved_topology != 0:
        return fail(66, "reserved_topology must be zero")

    # INV-01: cell threshold ordering
    if cfg.cell_uv_hard_mv >= cfg.cell_uv_soft_mv:
        return fail(68, "cell_uv_hard_mv must be < cell_uv_soft_mv")
    if cfg.cell_uv_soft_mv >= cfg.cell_balance_target_mv:
        return fail(70, "cell_uv_soft_mv must be < cell_balance_target_mv")
    if cfg.cell_balance_target_mv >= cfg.cell_ov_soft_mv:
        return fail(76, "cell_balance_target_mv must be < cell_ov_soft_mv")
    if cfg.cell_ov_soft_mv >= cfg.cell_ov_hard_mv:
        return fail(72, "cell_ov_soft_mv must be < cell_ov_hard_mv")
    if cfg.cell_balance_hysteresis_mv >= (cfg.cell_ov_soft_mv - cfg.cell_balance_target_mv):
        return fail(78, "cell_balance_hysteresis_mv too large")

    # INV-02
    if cfg.temp_charge_warn_cx10 >= cfg.temp_charge_hard_cx10:
        return fail(84, "temp_charge_warn >= temp_charge_hard")
    if cfg.temp_charge_hard_cx10 > cfg.temp_hard_abs_cx10:
        return fail(86, "temp_charge_hard > temp_hard_abs")

    # INV-03
    if cfg.temp_discharge_warn_cx10 >= cfg.temp_discharge_hard_cx10:
        return fail(88, "temp_discharge_warn >= temp_discharge_hard")
    if cfg.temp_discharge_hard_cx10 > cfg.temp_hard_abs_cx10:
        return fail(90, "temp_discharge_hard > temp_hard_abs")

    # INV-04
    if cfg.temp_cold_discharge_limit_cx10 > cfg.temp_cold_charge_limit_cx10:
        return fail(96, "cold_discharge_limit > cold_charge_limit")

    # INV-05
    if cfg.overcurrent_hard_ma == 0:
        return fail(100, "overcurrent_hard_ma must be > 0")
    if cfg.overcurrent_warn_ma > cfg.overcurrent_hard_ma:
        return fail(104, "overcurrent_warn > overcurrent_hard")

    if cfg.precharge_pct < 50 or cfg.precharge_pct > 99:
        return fail(108, "precharge_pct must be in [50, 99]")
    if cfg.precharge_timeout_ms == 0:
        return fail(110, "precharge_timeout_ms must be > 0")
    if cfg.precharge_delta_max_pct < 1 or cfg.precharge_delta_max_pct > 20:
        return fail(114, "precharge_delta_max_pct must be in [1, 20]")
    if cfg.balance_on_time_ms == 0:
        return fail(116, "balance_on_time_ms must be > 0")
    if cfg.balance_off_time_ms == 0:
        return fail(120, "balance_off_time_ms must be > 0")
    if cfg.temp_settle_time_ms == 0:
        return fail(124, "temp_settle_time_ms must be > 0")
    if cfg.stale_data_timeout_ms < 100:
        return fail(128, "stale_data_timeout_ms must be >= 100")

    # INV-06: mask reserved bits
    if cfg.required_cell_mask[9] & 0xF8:
        return fail(132, "required_cell_mask bits 75-79 must be zero")
    if cfg.required_temp_mask[9] & 0xF8:
        return fail(142, "required_temp_mask bits 75-79 must be zero")
    if cfg.balance_allowed_mask[9] & 0xF8:
        return fail(152, "balance_allowed_mask bits 75-79 must be zero")

    if cfg.vpack_gain_x1000 == 0:
        return fail(162, "vpack_gain_x1000 must be > 0")
    if cfg.vbat_gain_x1000 == 0:
        return fail(170, "vbat_gain_x1000 must be > 0")
    if cfg.current_gain_x1000 == 0:
        return fail(174, "current_gain_x1000 must be > 0")
    if cfg.can_base_id > 0x7FF:
        return fail(182, "can_base_id must be <= 0x7FF")

    if cfg.capacity_mah == 0:
        return fail(188, "capacity_mah must be > 0")

    return True, 0xFFFF, "OK"
