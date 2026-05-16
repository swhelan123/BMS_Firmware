"""target_model.py — high-level BMS target interface with safety enforcement.

All public methods raise TargetRefusedError when safety rules block the operation.
Call capabilities_handshake() immediately after connecting.
"""
import struct
from typing import Optional, Tuple

from ..protocol.client import BmsProtocolClient, ProtocolError
from ..protocol.packet_defs import (
    HW_PROFILE_ID, PROTOCOL_VERSION,
    FIRMWARE_TYPE_BMS_APP, FIRMWARE_TYPE_BOOTLOADER,
    PKT_GET_DIAGNOSTICS_SUMMARY, PKT_RUN_OPENWIRE,
)
from ..connection.device_state import DeviceState, DeviceMode, CapabilitiesState
from ..config.schema import BmsConfig
from ..config.validator import validate_config as _validate_local
from .app_state import (
    ValuesState, CellsState, TempsState, FaultsState, DiagnosticsState,
)


class TargetRefusedError(Exception):
    """Raised when the backend refuses an operation due to safety or compatibility rules."""


class TargetModel:
    """Wraps BmsProtocolClient with capabilities tracking and safety enforcement."""

    def __init__(self, port):
        self._client = BmsProtocolClient(port)
        self._caps:   Optional[CapabilitiesState] = None
        self._device  = DeviceState()

    @property
    def device(self) -> DeviceState:
        return self._device

    @property
    def capabilities(self) -> Optional[CapabilitiesState]:
        return self._caps

    # ── Handshake ─────────────────────────────────────────────────────────────

    def capabilities_handshake(self) -> DeviceState:
        """Perform GET_CAPABILITIES and determine device mode. Always call first."""
        try:
            raw = self._client.get_capabilities()
        except ProtocolError as e:
            self._device = DeviceState(mode=DeviceMode.UNKNOWN, error_msg=str(e))
            return self._device

        fw_type    = raw['firmware_type']
        hw_profile = raw['hw_profile_id']
        proto_ver  = raw['protocol_version']

        caps = CapabilitiesState(
            firmware_type         = fw_type,
            firmware_version      = raw['firmware_version'],
            hw_profile_id         = hw_profile,
            protocol_version      = proto_ver,
            config_schema_version = raw['config_schema_version'],
            cell_count            = raw['cell_count'],
            temp_count            = raw['temp_count'],
            feature_flags         = raw['feature_flags'],
        )

        if hw_profile != HW_PROFILE_ID:
            mode = DeviceMode.UNSUPPORTED
            msg  = (f"hw_profile mismatch: got 0x{hw_profile:04X}, "
                    f"expected 0x{HW_PROFILE_ID:04X}")
        elif proto_ver != PROTOCOL_VERSION:
            mode = DeviceMode.UNSUPPORTED
            msg  = (f"protocol_version mismatch: got {proto_ver}, "
                    f"expected {PROTOCOL_VERSION}")
        elif fw_type == FIRMWARE_TYPE_BMS_APP:
            mode, msg = DeviceMode.BMS_APP,    ''
        elif fw_type == FIRMWARE_TYPE_BOOTLOADER:
            mode, msg = DeviceMode.BOOTLOADER, ''
        else:
            mode = DeviceMode.UNKNOWN
            msg  = f"Unrecognised firmware_type 0x{fw_type:04X}"

        self._caps   = caps
        self._device = DeviceState(mode=mode, capabilities=caps, error_msg=msg)
        return self._device

    # ── Safety guards ─────────────────────────────────────────────────────────

    def _require_app_mode(self) -> None:
        if self._device.mode != DeviceMode.BMS_APP:
            raise TargetRefusedError(
                f"Requires BMS_APP mode; current: {self._device.mode.name}")

    def _require_valid_profile(self) -> None:
        if self._caps is None:
            raise TargetRefusedError("Capabilities not established — call capabilities_handshake()")
        if self._caps.hw_profile_id != HW_PROFILE_ID:
            raise TargetRefusedError(
                f"hw_profile mismatch: 0x{self._caps.hw_profile_id:04X}")

    # ── Polling ───────────────────────────────────────────────────────────────

    def poll_values(self) -> ValuesState:
        self._require_app_mode()
        r = self._client.get_values()
        return ValuesState(
            vbat_mv           = r['vbat_mv'],
            vpack_mv          = r['vpack_mv'],
            i_batt_ma         = r['i_batt_ma'],
            bms_state         = r['state'],
            active_faults     = r['active_faults'],
            latched_faults    = r['latched_faults'],
            outputs_state     = r['outputs_state'],
            uptime_ms         = r['uptime_ms'],
            measurement_flags = r['measurement_flags'],
            valid             = True,
        )

    def poll_cells(self) -> CellsState:
        self._require_app_mode()
        r = self._client.get_cells(include_validity=True)
        return CellsState(
            cell_count   = r['cell_count'],
            cells_mv     = r['cells_mv'],
            validity     = r.get('validity'),
            timestamp_ms = r['timestamp_ms'],
            valid        = True,
        )

    def poll_temps(self) -> TempsState:
        self._require_app_mode()
        r = self._client.get_temps()
        return TempsState(
            temp_count = r['temp_count'],
            temps_cx10 = r['temps_cx10'],
            valid      = True,
        )

    def poll_faults(self) -> FaultsState:
        self._require_app_mode()
        r = self._client.get_faults()
        return FaultsState(
            active_faults  = r['active_faults'],
            latched_faults = r['latched_faults'],
            valid          = True,
        )

    def poll_diagnostics(self) -> DiagnosticsState:
        self._require_app_mode()
        p = self._client.send_request(PKT_GET_DIAGNOSTICS_SUMMARY)
        if len(p) < 28:
            raise ProtocolError(f"DIAGNOSTICS_SUMMARY too short: {len(p)} bytes")
        return DiagnosticsState(
            reset_cause     = p[0],
            pec_cell_errors = struct.unpack_from('<I', p, 1)[0],
            pec_temp_errors = struct.unpack_from('<I', p, 5)[0],
            i2c_errors      = struct.unpack_from('<I', p, 9)[0],
            open_wire_valid = bool(p[13]),
            open_wire_mask  = bytes(p[14:24]),
            uptime_ms       = struct.unpack_from('<I', p, 24)[0],
            valid           = True,
        )

    # ── Config ────────────────────────────────────────────────────────────────

    def read_config(self) -> BmsConfig:
        self._require_app_mode()
        self._require_valid_profile()
        return BmsConfig.unpack(self._client.get_config())

    def validate_config_offline(self, cfg: BmsConfig) -> Tuple[bool, int, str]:
        ok, err_off, msg = _validate_local(cfg)
        return ok, err_off, msg

    def validate_config_on_target(self, cfg: BmsConfig) -> Tuple[bool, int, str]:
        self._require_app_mode()
        self._require_valid_profile()
        ok, err_off = self._client.validate_config(cfg.pack())
        msg = "OK" if ok else f"Validation failed at offset 0x{err_off:04X}"
        return ok, err_off, msg

    def apply_config_ram(self, cfg: BmsConfig) -> Tuple[bool, int, str]:
        self._require_app_mode()
        self._require_valid_profile()
        ok, err_off = self._client.set_config_ram(cfg.pack())
        msg = "OK" if ok else f"RAM apply failed at offset 0x{err_off:04X}"
        return ok, err_off, msg

    def store_config(self, cfg: BmsConfig) -> bool:
        self._require_app_mode()
        self._require_valid_profile()
        return self._client.store_config(cfg.pack())

    def clear_latched_faults(self, mask: int) -> int:
        self._require_app_mode()
        return self._client.clear_latched_faults(mask)

    def enter_bootloader(self) -> None:
        """Send ENTER_BOOTLOADER. Caller must call capabilities_handshake() again after."""
        self._require_app_mode()
        self._client.enter_bootloader()

    # ── Diagnostics ───────────────────────────────────────────────────────────

    def run_openwire(self) -> dict:
        self._require_app_mode()
        p = self._client.send_request(PKT_RUN_OPENWIRE)
        if len(p) < 11:
            raise ProtocolError(f"RUN_OPENWIRE response too short: {len(p)}")
        return {'status': p[0], 'open_wire_mask': bytes(p[1:11])}

    # ── Package compatibility ─────────────────────────────────────────────────

    def validate_package_against_target(self, pkg_header) -> Tuple[bool, str]:
        if self._caps is None:
            return False, "Capabilities not established"
        if pkg_header.hw_profile_id != self._caps.hw_profile_id:
            return False, (
                f"Package hw_profile 0x{pkg_header.hw_profile_id:04X} "
                f"!= target 0x{self._caps.hw_profile_id:04X}")
        return True, "OK"
