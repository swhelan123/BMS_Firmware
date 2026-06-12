"""validator.py — client-side config validation (mirrors firmware bms_config_validate).

Returns (ok: bool, error_field_offset: int, error_message: str).
Error offsets come from schema.FIELD_OFFSETS so they always match the
struct layout — never hand-typed.
"""
from .schema import (
    BmsConfig, CONFIG_MAGIC, SCHEMA_VERSION, CONFIG_SCHEMA_SIZE, FIELD_OFFSETS,
)
from ..protocol.packet_defs import HW_PROFILE_ID
from ..protocol.crc import crc32_iso_hdlc
import struct


def validate_config(cfg: BmsConfig) -> tuple:
    """Returns (ok, err_offset, message)."""
    def fail(field_name, msg):
        return False, FIELD_OFFSETS[field_name], msg

    if cfg.magic != CONFIG_MAGIC:
        return fail('magic', "Wrong magic")
    if cfg.schema_version != SCHEMA_VERSION:
        return fail('schema_version', "Wrong schema_version")
    if cfg.total_length != CONFIG_SCHEMA_SIZE:
        return fail('total_length', "Wrong total_length")
    if cfg.hw_profile_id != HW_PROFILE_ID:
        return fail('hw_profile_id', f"Wrong hw_profile_id: got 0x{cfg.hw_profile_id:04X}")
    if cfg.config_generation == 0xFFFFFFFF:
        return fail('config_generation', "Invalid config_generation")

    # Verify CRC
    blob = cfg.pack()  # pack() zeroes CRC field during computation
    expected_crc = struct.unpack_from('<I', blob, FIELD_OFFSETS['config_crc32'])[0]
    if expected_crc != cfg.config_crc32 and cfg.config_crc32 != 0:
        return fail('config_crc32', "CRC mismatch")

    if cfg.reserved_header != bytes(46):
        return fail('reserved_header', "reserved_header must be zero")
    if cfg.cell_count != 75:
        return fail('cell_count', "cell_count must be 75")
    if cfg.temp_count != 75:
        return fail('temp_count', "temp_count must be 75")
    if cfg.reserved_topology != 0:
        return fail('reserved_topology', "reserved_topology must be zero")

    # INV-01: cell threshold ordering
    if cfg.cell_uv_hard_mv >= cfg.cell_uv_soft_mv:
        return fail('cell_uv_hard_mv', "cell_uv_hard_mv must be < cell_uv_soft_mv")
    if cfg.cell_uv_soft_mv >= cfg.cell_balance_target_mv:
        return fail('cell_uv_soft_mv', "cell_uv_soft_mv must be < cell_balance_target_mv")
    if cfg.cell_balance_target_mv >= cfg.cell_ov_soft_mv:
        return fail('cell_balance_target_mv', "cell_balance_target_mv must be < cell_ov_soft_mv")
    if cfg.cell_ov_soft_mv >= cfg.cell_ov_hard_mv:
        return fail('cell_ov_soft_mv', "cell_ov_soft_mv must be < cell_ov_hard_mv")
    if cfg.cell_balance_hysteresis_mv >= (cfg.cell_ov_soft_mv - cfg.cell_balance_target_mv):
        return fail('cell_balance_hysteresis_mv', "cell_balance_hysteresis_mv too large")

    # INV-02
    if cfg.temp_charge_warn_cx10 >= cfg.temp_charge_hard_cx10:
        return fail('temp_charge_warn_cx10', "temp_charge_warn >= temp_charge_hard")
    if cfg.temp_charge_hard_cx10 > cfg.temp_hard_abs_cx10:
        return fail('temp_charge_hard_cx10', "temp_charge_hard > temp_hard_abs")

    # INV-03
    if cfg.temp_discharge_warn_cx10 >= cfg.temp_discharge_hard_cx10:
        return fail('temp_discharge_warn_cx10', "temp_discharge_warn >= temp_discharge_hard")
    if cfg.temp_discharge_hard_cx10 > cfg.temp_hard_abs_cx10:
        return fail('temp_discharge_hard_cx10', "temp_discharge_hard > temp_hard_abs")

    # INV-04
    if cfg.temp_cold_discharge_limit_cx10 > cfg.temp_cold_charge_limit_cx10:
        return fail('temp_cold_discharge_limit_cx10', "cold_discharge_limit > cold_charge_limit")

    # INV-05
    if cfg.overcurrent_hard_ma == 0:
        return fail('overcurrent_hard_ma', "overcurrent_hard_ma must be > 0")
    if cfg.overcurrent_warn_ma > cfg.overcurrent_hard_ma:
        return fail('overcurrent_warn_ma', "overcurrent_warn > overcurrent_hard")

    if cfg.balance_on_time_ms == 0:
        return fail('balance_on_time_ms', "balance_on_time_ms must be > 0")
    if cfg.balance_off_time_ms == 0:
        return fail('balance_off_time_ms', "balance_off_time_ms must be > 0")
    if cfg.temp_settle_time_ms == 0:
        return fail('temp_settle_time_ms', "temp_settle_time_ms must be > 0")
    if cfg.stale_data_timeout_ms < 100:
        return fail('stale_data_timeout_ms', "stale_data_timeout_ms must be >= 100")

    # INV-06: mask reserved bits
    if cfg.required_cell_mask[9] & 0xF8:
        return fail('required_cell_mask', "required_cell_mask bits 75-79 must be zero")
    if cfg.required_temp_mask[9] & 0xF8:
        return fail('required_temp_mask', "required_temp_mask bits 75-79 must be zero")
    if cfg.balance_allowed_mask[9] & 0xF8:
        return fail('balance_allowed_mask', "balance_allowed_mask bits 75-79 must be zero")

    if cfg.vpack_gain_x1000 == 0:
        return fail('vpack_gain_x1000', "vpack_gain_x1000 must be > 0")
    if cfg.vbat_gain_x1000 == 0:
        return fail('vbat_gain_x1000', "vbat_gain_x1000 must be > 0")
    if cfg.current_gain_x1000 == 0:
        return fail('current_gain_x1000', "current_gain_x1000 must be > 0")
    if cfg.can_base_id > 0x7FF:
        return fail('can_base_id', "can_base_id must be <= 0x7FF")

    if cfg.capacity_mah == 0:
        return fail('capacity_mah', "capacity_mah must be > 0")

    if cfg.reserved != bytes(42):
        return fail('reserved', "reserved must be zero")

    return True, 0xFFFF, "OK"
