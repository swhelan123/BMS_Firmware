"""test_protocol_sync.py — cross-checks that keep the Python tool, the YAML
protocol contract, and the firmware headers in lock-step.

This is the drift guard: a fault-bit renumber or config-struct change in any
one layer fails here until all layers agree. Added after the precharge
removal silently desynced the tool from the firmware.
"""
import re
import struct
from pathlib import Path

import pytest

yaml = pytest.importorskip("yaml")

from tool.src.protocol import bms_defs
from tool.src.config.schema import (
    BmsConfig, FIELD_OFFSETS, _FIELDS, _STRUCT_FMT,
)
from tool.src.protocol.packet_defs import CONFIG_SCHEMA_SIZE

REPO = Path(__file__).resolve().parents[2]
FAULT_YAML  = REPO / 'protocol' / 'fault_bits.yaml'
CONFIG_YAML = REPO / 'protocol' / 'config_schema.yaml'
TYPES_H     = REPO / 'firmware' / 'include' / 'bms_types.h'


# ── helpers ───────────────────────────────────────────────────────────────────

def _yaml_fault_bits():
    """bit → FAULT_NAME (without FAULT_ prefix) from fault_bits.yaml."""
    doc = yaml.safe_load(FAULT_YAML.read_text())
    out = {}
    for entry in doc['faults']:
        if 'bit' not in entry:        # reserved range entries use 'bits'
            continue
        name = entry['name']
        assert name.startswith('FAULT_'), name
        out[entry['bit']] = name[len('FAULT_'):]
    return out


def _yaml_config_fields():
    """field_name → (offset, size) from config_schema.yaml."""
    doc = yaml.safe_load(CONFIG_YAML.read_text())
    out = {}
    for group in doc['sections'].values():
        for f in group.get('fields', []):
            out[f['name']] = (f['offset'], f['size'])
    return out


def _header_enum(pattern):
    """name → int for every `pattern` match in bms_types.h."""
    text = TYPES_H.read_text()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(pattern, text)}


# ── fault bits: YAML ↔ bms_defs ↔ firmware header ────────────────────────────

class TestFaultBitSync:
    def test_yaml_matches_bms_defs_names(self):
        yaml_bits = _yaml_fault_bits()
        for bit, name in yaml_bits.items():
            assert bit < len(bms_defs.FAULT_NAMES), \
                f"YAML bit {bit} ({name}) beyond FAULT_NAMES table"
            assert bms_defs.FAULT_NAMES[bit] == name, \
                f"bit {bit}: YAML={name} bms_defs={bms_defs.FAULT_NAMES[bit]}"

    def test_bms_defs_has_no_extra_names(self):
        yaml_bits = _yaml_fault_bits()
        assert len(bms_defs.FAULT_NAMES) == len(yaml_bits), \
            "FAULT_NAMES length differs from YAML-defined bit count"

    def test_yaml_matches_firmware_header(self):
        yaml_bits = _yaml_fault_bits()
        fw_bits = _header_enum(r'FAULT_BIT_(\w+)\s*=\s*(\d+)')
        assert fw_bits, "no FAULT_BIT_ entries parsed from bms_types.h"
        for name, bit in fw_bits.items():
            assert yaml_bits.get(bit) == name, \
                f"firmware FAULT_BIT_{name}={bit} but YAML bit {bit} is {yaml_bits.get(bit)}"
        assert len(fw_bits) == len(yaml_bits)

    def test_bms_defs_constants_match_table(self):
        for bit, name in enumerate(bms_defs.FAULT_NAMES):
            const = getattr(bms_defs, f'FAULT_BIT_{name}')
            assert const == bit, f"FAULT_BIT_{name}={const}, table index {bit}"


# ── Permission blocking masks: bms_defs ↔ YAML permission_effect ─────────────

class TestBlockingMaskSync:
    _MASKS = {
        'master_ok':      'FAULT_BLOCKS_MASTER_OK_MASK',
        'discharge_perm': 'FAULT_BLOCKS_DISCHARGE_MASK',
        'charge_perm':    'FAULT_BLOCKS_CHARGE_MASK',
        'charger_safety': 'FAULT_BLOCKS_CHARGER_SAFETY_MASK',
    }

    def test_masks_match_yaml_permission_effects(self):
        doc = yaml.safe_load(FAULT_YAML.read_text())
        for perm, mask_name in self._MASKS.items():
            expected = 0
            for entry in doc['faults']:
                if 'bit' not in entry:
                    continue
                if perm in entry['permission_effect'].get('blocks', []):
                    expected |= 1 << entry['bit']
            actual = getattr(bms_defs, mask_name)
            assert actual == expected, (
                f"{mask_name}=0x{actual:X} but YAML blocks-list gives "
                f"0x{expected:X} (diff bits: "
                f"{bms_defs.fault_names_from_mask(actual ^ expected)})")


# ── BMS state enum: bms_defs ↔ firmware header ───────────────────────────────

class TestStateSync:
    def test_state_values_match_firmware(self):
        fw_states = _header_enum(r'BMS_STATE_(\w+)\s*=\s*(\d+)')
        assert fw_states, "no BMS_STATE_ entries parsed from bms_types.h"
        for name, value in fw_states.items():
            assert bms_defs.STATE_NAMES.get(value) == name, \
                f"firmware BMS_STATE_{name}={value} but STATE_NAMES[{value}]=" \
                f"{bms_defs.STATE_NAMES.get(value)}"
        assert len(fw_states) == len(bms_defs.STATE_NAMES)


# ── config struct: YAML ↔ schema.py ──────────────────────────────────────────

class TestConfigSchemaSync:
    def test_total_size(self):
        assert struct.calcsize(_STRUCT_FMT) == CONFIG_SCHEMA_SIZE == 226

    def test_yaml_offsets_match_schema(self):
        yaml_fields = _yaml_config_fields()
        for name, (offset, size) in yaml_fields.items():
            assert name in FIELD_OFFSETS, f"YAML field {name} missing from schema.py"
            assert FIELD_OFFSETS[name] == offset, \
                f"{name}: YAML offset {offset}, schema.py offset {FIELD_OFFSETS[name]}"
            code = dict(_FIELDS)[name]
            assert struct.calcsize('<' + code) == size, \
                f"{name}: YAML size {size}, schema.py size {struct.calcsize('<' + code)}"

    def test_schema_has_no_extra_fields(self):
        yaml_fields = _yaml_config_fields()
        extra = set(FIELD_OFFSETS) - set(yaml_fields)
        assert not extra, f"schema.py fields not in YAML: {sorted(extra)}"

    def test_pack_unpack_roundtrip(self):
        cfg = BmsConfig()
        blob = cfg.pack()
        assert len(blob) == CONFIG_SCHEMA_SIZE
        cfg2 = BmsConfig.unpack(blob)
        blob2 = cfg2.pack()
        assert blob == blob2

    def test_default_config_validates(self):
        from tool.src.config.validator import validate_config
        cfg = BmsConfig.unpack(BmsConfig().pack())
        ok, off, msg = validate_config(cfg)
        assert ok, f"default config invalid at offset {off}: {msg}"
