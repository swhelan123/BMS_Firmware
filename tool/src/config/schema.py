"""schema.py — BmsConfig struct serialize/deserialize.

Matches bms_config.h BmsConfig struct exactly (226 bytes, packed, little-endian).
Generated from: protocol/config_schema.yaml
"""
import struct
from dataclasses import dataclass, field
from typing import List

from ..protocol.crc import crc32_iso_hdlc
from ..protocol.packet_defs import HW_PROFILE_ID, CONFIG_SCHEMA_SIZE

CONFIG_MAGIC   = 0xBBCC0001
SCHEMA_VERSION = 1

# Struct format: all fields in LE order, packed.
# 226 bytes total.
_STRUCT_FMT = '<'
_STRUCT_FMT += 'I'   # magic                       0
_STRUCT_FMT += 'H'   # schema_version              4
_STRUCT_FMT += 'H'   # total_length                6
_STRUCT_FMT += 'H'   # hw_profile_id               8
_STRUCT_FMT += 'I'   # config_generation           10
_STRUCT_FMT += 'I'   # config_crc32                14
_STRUCT_FMT += '46s' # reserved_header             18
_STRUCT_FMT += 'B'   # cell_count                  64
_STRUCT_FMT += 'B'   # temp_count                  65
_STRUCT_FMT += 'H'   # reserved_topology           66
_STRUCT_FMT += 'H'   # cell_uv_hard_mv             68
_STRUCT_FMT += 'H'   # cell_uv_soft_mv             70
_STRUCT_FMT += 'H'   # cell_ov_soft_mv             72
_STRUCT_FMT += 'H'   # cell_ov_hard_mv             74
_STRUCT_FMT += 'H'   # cell_balance_target_mv      76
_STRUCT_FMT += 'H'   # cell_balance_hysteresis_mv  78
_STRUCT_FMT += 'H'   # cell_nominal_mv             80
_STRUCT_FMT += 'H'   # reserved_cell_thresholds    82
_STRUCT_FMT += 'h'   # temp_charge_warn_cx10       84
_STRUCT_FMT += 'h'   # temp_charge_hard_cx10       86
_STRUCT_FMT += 'h'   # temp_discharge_warn_cx10    88
_STRUCT_FMT += 'h'   # temp_discharge_hard_cx10    90
_STRUCT_FMT += 'h'   # temp_hard_abs_cx10          92
_STRUCT_FMT += 'h'   # temp_cold_charge_limit_cx10 94
_STRUCT_FMT += 'h'   # temp_cold_discharge_limit   96
_STRUCT_FMT += 'H'   # reserved_temp_thresholds    98
_STRUCT_FMT += 'I'   # overcurrent_hard_ma         100
_STRUCT_FMT += 'I'   # overcurrent_warn_ma         104
_STRUCT_FMT += 'H'   # precharge_pct               108
_STRUCT_FMT += 'I'   # precharge_timeout_ms        110
_STRUCT_FMT += 'H'   # precharge_delta_max_pct     114
_STRUCT_FMT += 'I'   # balance_on_time_ms          116
_STRUCT_FMT += 'I'   # balance_off_time_ms         120
_STRUCT_FMT += 'H'   # temp_settle_time_ms         124
_STRUCT_FMT += 'H'   # reserved_temp_params        126
_STRUCT_FMT += 'I'   # stale_data_timeout_ms       128
_STRUCT_FMT += '10s' # required_cell_mask          132
_STRUCT_FMT += '10s' # required_temp_mask          142
_STRUCT_FMT += '10s' # balance_allowed_mask        152
_STRUCT_FMT += 'I'   # vpack_gain_x1000            162
_STRUCT_FMT += 'i'   # vpack_offset_mv             166
_STRUCT_FMT += 'H'   # vbat_gain_x1000             170
_STRUCT_FMT += 'h'   # vbat_offset_mv              172
_STRUCT_FMT += 'I'   # current_gain_x1000          174  (uint32: AMC1302 chain needs ~1,855,000)
_STRUCT_FMT += 'h'   # current_offset_ma           178
_STRUCT_FMT += 'I'   # can_watchdog_timeout_ms     180
_STRUCT_FMT += 'H'   # can_base_id                 184
_STRUCT_FMT += 'H'   # reserved_can                186
_STRUCT_FMT += 'I'   # capacity_mah                188
_STRUCT_FMT += '34s' # reserved                    192

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
    # Precharge
    precharge_pct:                  int   = 90
    precharge_timeout_ms:           int   = 10000
    precharge_delta_max_pct:        int   = 5
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
    reserved:                       bytes = field(default_factory=lambda: bytes(34))

    def pack(self) -> bytes:
        """Serialize to 226-byte blob with correct CRC."""
        blob = struct.pack(_STRUCT_FMT,
            self.magic, self.schema_version, self.total_length,
            self.hw_profile_id, self.config_generation, 0,  # crc=0 for computation
            self.reserved_header,
            self.cell_count, self.temp_count, self.reserved_topology,
            self.cell_uv_hard_mv, self.cell_uv_soft_mv, self.cell_ov_soft_mv,
            self.cell_ov_hard_mv, self.cell_balance_target_mv,
            self.cell_balance_hysteresis_mv, self.cell_nominal_mv,
            self.reserved_cell_thresholds,
            self.temp_charge_warn_cx10, self.temp_charge_hard_cx10,
            self.temp_discharge_warn_cx10, self.temp_discharge_hard_cx10,
            self.temp_hard_abs_cx10, self.temp_cold_charge_limit_cx10,
            self.temp_cold_discharge_limit_cx10, self.reserved_temp_thresholds,
            self.overcurrent_hard_ma, self.overcurrent_warn_ma,
            self.precharge_pct, self.precharge_timeout_ms,
            self.precharge_delta_max_pct,
            self.balance_on_time_ms, self.balance_off_time_ms,
            self.temp_settle_time_ms, self.reserved_temp_params,
            self.stale_data_timeout_ms,
            self.required_cell_mask, self.required_temp_mask,
            self.balance_allowed_mask,
            self.vpack_gain_x1000, self.vpack_offset_mv,
            self.vbat_gain_x1000, self.vbat_offset_mv,
            self.current_gain_x1000, self.current_offset_ma,
            self.can_watchdog_timeout_ms, self.can_base_id, self.reserved_can,
            self.capacity_mah,
            self.reserved,
        )
        crc = crc32_iso_hdlc(blob)
        # Inject CRC at offset 14
        return blob[:14] + struct.pack('<I', crc) + blob[18:]

    @staticmethod
    def unpack(data: bytes) -> 'BmsConfig':
        if len(data) != CONFIG_SCHEMA_SIZE:
            raise ValueError(f"Expected {CONFIG_SCHEMA_SIZE} bytes, got {len(data)}")
        fields = struct.unpack(_STRUCT_FMT, data)
        return BmsConfig(
            magic=fields[0], schema_version=fields[1], total_length=fields[2],
            hw_profile_id=fields[3], config_generation=fields[4], config_crc32=fields[5],
            reserved_header=fields[6],
            cell_count=fields[7], temp_count=fields[8], reserved_topology=fields[9],
            cell_uv_hard_mv=fields[10], cell_uv_soft_mv=fields[11],
            cell_ov_soft_mv=fields[12], cell_ov_hard_mv=fields[13],
            cell_balance_target_mv=fields[14], cell_balance_hysteresis_mv=fields[15],
            cell_nominal_mv=fields[16], reserved_cell_thresholds=fields[17],
            temp_charge_warn_cx10=fields[18], temp_charge_hard_cx10=fields[19],
            temp_discharge_warn_cx10=fields[20], temp_discharge_hard_cx10=fields[21],
            temp_hard_abs_cx10=fields[22], temp_cold_charge_limit_cx10=fields[23],
            temp_cold_discharge_limit_cx10=fields[24], reserved_temp_thresholds=fields[25],
            overcurrent_hard_ma=fields[26], overcurrent_warn_ma=fields[27],
            precharge_pct=fields[28], precharge_timeout_ms=fields[29],
            precharge_delta_max_pct=fields[30],
            balance_on_time_ms=fields[31], balance_off_time_ms=fields[32],
            temp_settle_time_ms=fields[33], reserved_temp_params=fields[34],
            stale_data_timeout_ms=fields[35],
            required_cell_mask=fields[36], required_temp_mask=fields[37],
            balance_allowed_mask=fields[38],
            vpack_gain_x1000=fields[39], vpack_offset_mv=fields[40],
            vbat_gain_x1000=fields[41], vbat_offset_mv=fields[42],
            current_gain_x1000=fields[43], current_offset_ma=fields[44],
            can_watchdog_timeout_ms=fields[45], can_base_id=fields[46],
            reserved_can=fields[47], capacity_mah=fields[48], reserved=fields[49],
        )
