"""test_config_validator.py — Python config validator tests, mirrors firmware validation."""
import pytest
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from tool.src.config.schema import BmsConfig
from tool.src.config.validator import validate_config
from tool.src.config.schema import FIELD_OFFSETS


def make_valid() -> BmsConfig:
    return BmsConfig()

def test_default_config_passes():
    ok, _, _ = validate_config(make_valid())
    assert ok

def test_wrong_magic_fails():
    cfg = make_valid()
    cfg.magic = 0xDEADBEEF
    ok, off, msg = validate_config(cfg)
    assert not ok
    assert off == 0

def test_wrong_hw_profile_fails():
    cfg = make_valid()
    cfg.hw_profile_id = 0x9999
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == 8

def test_inverted_uv_ov_fails():
    cfg = make_valid()
    cfg.cell_uv_hard_mv, cfg.cell_ov_hard_mv = cfg.cell_ov_hard_mv, cfg.cell_uv_hard_mv
    ok, _, _ = validate_config(cfg)
    assert not ok

def test_mask_top_bits_fails():
    cfg = make_valid()
    mask = bytearray(cfg.required_cell_mask)
    mask[9] |= 0x80
    cfg.required_cell_mask = bytes(mask)
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == FIELD_OFFSETS['required_cell_mask']

def test_pack_unpack_roundtrip():
    cfg = make_valid()
    blob = cfg.pack()
    cfg2 = BmsConfig.unpack(blob)
    assert cfg2.cell_uv_hard_mv == cfg.cell_uv_hard_mv
    assert cfg2.can_base_id == cfg.can_base_id
    assert len(blob) == 226

def test_invalid_cell_count_fails():
    cfg = make_valid()
    cfg.cell_count = 70
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == 64


def _segment_mask(count: int) -> bytes:
    """10-byte mask with bits [0..count) set, matching an N-segment pack."""
    m = bytearray(10)
    for i in range(count):
        m[i // 8] |= (1 << (i % 8))
    return bytes(m)

def _make_segments(count: int) -> BmsConfig:
    cfg = make_valid()
    cfg.cell_count = count
    cfg.temp_count = count
    cfg.required_cell_mask   = _segment_mask(count)
    cfg.required_temp_mask   = _segment_mask(count)
    cfg.balance_allowed_mask = _segment_mask(count)
    return cfg

def test_60cell_config_passes():
    ok, _, _ = validate_config(_make_segments(60))
    assert ok

def test_non_segment_count_fails():
    cfg = _make_segments(60)
    cfg.cell_count = 61  # not a multiple of 15
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == 64

def test_cell_temp_mismatch_fails():
    cfg = _make_segments(60)
    cfg.temp_count = 75
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == 65

def test_required_mask_beyond_count_fails():
    cfg = _make_segments(60)
    mask = bytearray(cfg.required_cell_mask)
    mask[7] |= (1 << 4)  # bit 60 — absent cell
    cfg.required_cell_mask = bytes(mask)
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == FIELD_OFFSETS['required_cell_mask']

def test_temp_ordering_fails():
    cfg = make_valid()
    cfg.temp_charge_warn_cx10 = cfg.temp_charge_hard_cx10  # equal → fail
    ok, _, _ = validate_config(cfg)
    assert not ok

def test_overcurrent_zero_fails():
    cfg = make_valid()
    cfg.overcurrent_hard_ma = 0
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == 100

def test_can_id_out_of_range_fails():
    cfg = make_valid()
    cfg.can_base_id = 0x800  # > 11-bit max
    ok, off, _ = validate_config(cfg)
    assert not ok
    assert off == FIELD_OFFSETS['can_base_id']
