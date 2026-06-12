"""schema.py — BmsConfig struct serialize/deserialize.

Matches bms_config.h BmsConfig struct exactly (226 bytes, packed, little-endian).
Source of truth: protocol/config_schema.yaml — tool/tests/test_protocol_sync.py
asserts the field table below stays in lock-step with the YAML.
"""
import struct
from dataclasses import dataclass, field
from typing import List

from ..protocol.crc import crc32_iso_hdlc
from ..protocol.packet_defs import HW_PROFILE_ID, CONFIG_SCHEMA_SIZE

CONFIG_MAGIC   = 0xBBCC0001
SCHEMA_VERSION = 1

# (field_name, struct_code) in storage order. Offsets are derived, not typed
# by hand — see FIELD_OFFSETS below and the sync test.
_FIELDS = [
    ('magic',                          'I'),    # 0
    ('schema_version',                 'H'),    # 4
    ('total_length',                   'H'),    # 6
    ('hw_profile_id',                  'H'),    # 8
    ('config_generation',              'I'),    # 10
    ('config_crc32',                   'I'),    # 14
    ('reserved_header',                '46s'),  # 18
    ('cell_count',                     'B'),    # 64
    ('temp_count',                     'B'),    # 65
    ('reserved_topology',              'H'),    # 66
    ('cell_uv_hard_mv',                'H'),    # 68
    ('cell_uv_soft_mv',                'H'),    # 70
    ('cell_ov_soft_mv',                'H'),    # 72
    ('cell_ov_hard_mv',                'H'),    # 74
    ('cell_balance_target_mv',         'H'),    # 76
    ('cell_balance_hysteresis_mv',     'H'),    # 78
    ('cell_nominal_mv',                'H'),    # 80
    ('reserved_cell_thresholds',       'H'),    # 82
    ('temp_charge_warn_cx10',          'h'),    # 84
    ('temp_charge_hard_cx10',          'h'),    # 86
    ('temp_discharge_warn_cx10',       'h'),    # 88
    ('temp_discharge_hard_cx10',       'h'),    # 90
    ('temp_hard_abs_cx10',             'h'),    # 92
    ('temp_cold_charge_limit_cx10',    'h'),    # 94
    ('temp_cold_discharge_limit_cx10', 'h'),    # 96
    ('reserved_temp_thresholds',       'H'),    # 98
    ('overcurrent_hard_ma',            'I'),    # 100
    ('overcurrent_warn_ma',            'I'),    # 104
    ('balance_on_time_ms',             'I'),    # 108
    ('balance_off_time_ms',            'I'),    # 112
    ('temp_settle_time_ms',            'H'),    # 116
    ('reserved_temp_params',           'H'),    # 118
    ('stale_data_timeout_ms',          'I'),    # 120
    ('required_cell_mask',             '10s'),  # 124
    ('required_temp_mask',             '10s'),  # 134
    ('balance_allowed_mask',           '10s'),  # 144
    ('vpack_gain_x1000',               'I'),    # 154
    ('vpack_offset_mv',                'i'),    # 158
    ('vbat_gain_x1000',                'H'),    # 162
    ('vbat_offset_mv',                 'h'),    # 164
    ('current_gain_x1000',             'I'),    # 166  (uint32: AMC1302 chain needs ~1,855,000)
    ('current_offset_ma',              'h'),    # 170
    ('can_watchdog_timeout_ms',        'I'),    # 172
    ('can_base_id',                    'H'),    # 176
    ('reserved_can',                   'H'),    # 178
    ('capacity_mah',                   'I'),    # 180
    ('reserved',                       '42s'),  # 184
]

_FIELD_NAMES = [name for name, _ in _FIELDS]
_STRUCT_FMT  = '<' + ''.join(code for _, code in _FIELDS)

# name → byte offset, derived from the format codes (single source of truth)
FIELD_OFFSETS = {}
_off = 0
for _name, _code in _FIELDS:
    FIELD_OFFSETS[_name] = _off
    _off += struct.calcsize('<' + _code)
del _off, _name, _code

assert struct.calcsize(_STRUCT_FMT) == CONFIG_SCHEMA_SIZE, \
    f"Schema struct size mismatch: {struct.calcsize(_STRUCT_FMT)} != {CONFIG_SCHEMA_SIZE}"

_ALL_75_BITS = bytes([0xFF]*9 + [0x07])  # bits 0-74 set, 75-79 clear


@dataclass
class BmsConfig:
    # Header
    magic:                          int   = CONFIG_MAGIC
    schema_version:                 int   = SCHEMA_VERSION
    total_length:                   int   = CONFIG_SCHEMA_SIZE
    hw_profile_id:                  int   = HW_PROFILE_ID
    config_generation:              int   = 1
    config_crc32:                   int   = 0
    reserved_header:                bytes = field(default_factory=lambda: bytes(46))
    # Topology
    cell_count:                     int   = 75
    temp_count:                     int   = 75
    reserved_topology:              int   = 0
    # Cell thresholds
    cell_uv_hard_mv:                int   = 2750
    cell_uv_soft_mv:                int   = 3000
    cell_ov_soft_mv:                int   = 4150
    cell_ov_hard_mv:                int   = 4200
    cell_balance_target_mv:         int   = 3800
    cell_balance_hysteresis_mv:     int   = 10
    cell_nominal_mv:                int   = 3700
    reserved_cell_thresholds:       int   = 0
    # Temp thresholds
    temp_charge_warn_cx10:          int   = 400
    temp_charge_hard_cx10:          int   = 450
    temp_discharge_warn_cx10:       int   = 550
    temp_discharge_hard_cx10:       int   = 600
    temp_hard_abs_cx10:             int   = 700
    temp_cold_charge_limit_cx10:    int   = 0
    temp_cold_discharge_limit_cx10: int   = -200
    reserved_temp_thresholds:       int   = 0
    # Current
    overcurrent_hard_ma:            int   = 100000
    overcurrent_warn_ma:            int   = 80000
    # Balancing
    balance_on_time_ms:             int   = 5000
    balance_off_time_ms:            int   = 1000
    # Temp measurement
    temp_settle_time_ms:            int   = 5
    reserved_temp_params:           int   = 0
    # Stale data
    stale_data_timeout_ms:          int   = 500
    # Masks
    required_cell_mask:             bytes = field(default_factory=lambda: _ALL_75_BITS)
    required_temp_mask:             bytes = field(default_factory=lambda: _ALL_75_BITS)
    balance_allowed_mask:           bytes = field(default_factory=lambda: _ALL_75_BITS)
    # Calibration — theoretical pre-hardware values; refine with bench measurement
    vpack_gain_x1000:               int   = 50706   # 4×470k÷1k → AMC1301(8.2) → OPA(5.893) → 33k/43k
    vpack_offset_mv:                int   = 0
    vbat_gain_x1000:                int   = 2000    # R43/R44 (3.3k/3.3k ÷2 divider) → ISL28022 Vbus
    vbat_offset_mv:                 int   = 0
    current_gain_x1000:             int   = 1000    # placeholder — 0.1mΩ shunt+AMC1302+÷7.6 ≈ 1,855,000
    current_offset_ma:              int   = 0
    # CAN
    can_watchdog_timeout_ms:        int   = 0
    can_base_id:                    int   = 0x0500
    reserved_can:                   int   = 0
    # Capacity
    capacity_mah:                   int   = 100000  # 100 Ah default — adjust per pack
    # Reserved
    reserved:                       bytes = field(default_factory=lambda: bytes(42))

    def pack(self) -> bytes:
        """Serialize to 226-byte blob with correct CRC."""
        values = []
        for name in _FIELD_NAMES:
            values.append(0 if name == 'config_crc32' else getattr(self, name))
        blob = struct.pack(_STRUCT_FMT, *values)
        crc = crc32_iso_hdlc(blob)
        # Inject CRC at its field offset
        off = FIELD_OFFSETS['config_crc32']
        return blob[:off] + struct.pack('<I', crc) + blob[off + 4:]

    @staticmethod
    def unpack(data: bytes) -> 'BmsConfig':
        if len(data) != CONFIG_SCHEMA_SIZE:
            raise ValueError(f"Expected {CONFIG_SCHEMA_SIZE} bytes, got {len(data)}")
        values = struct.unpack(_STRUCT_FMT, data)
        return BmsConfig(**dict(zip(_FIELD_NAMES, values)))
