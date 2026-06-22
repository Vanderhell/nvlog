from __future__ import annotations

import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bench_common import fault_mechanism_from_backend, parse_protocol_line, validate_config


class BenchCommonTests(unittest.TestCase):
    def test_parse_protocol_line_accepts_nvlog_prefix(self) -> None:
        parsed = parse_protocol_line("NVLOG|SCENARIO_PASS|id=single_append")
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed["event"], "SCENARIO_PASS")
        self.assertEqual(parsed["fields"]["id"], "single_append")

    def test_parse_protocol_line_accepts_legacy_prefix(self) -> None:
        parsed = parse_protocol_line("@HWTEST|READY")
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed["event"], "READY")

    def test_fault_mechanism_from_backend(self) -> None:
        self.assertEqual(fault_mechanism_from_backend("command"), "USB_POWER_CUT")
        self.assertEqual(
            fault_mechanism_from_backend("windows_pnp"), "USB_LOGICAL_DISCONNECT"
        )

    def test_validate_config_accepts_fixture_shape(self) -> None:
        config = {
            "session": {
                "duration_seconds": 10,
                "output_directory": "sessions",
                "startup_settle_seconds": 1.0,
                "power_worker_stop_timeout_seconds": 5,
                "abort_on_power_worker_failure": True,
            },
            "serial": {
                "port": "COM19",
                "strict_configured_port": False,
                "baudrate": 115200,
                "read_timeout_seconds": 0.25,
                "write_timeout_seconds": 2,
                "reconnect_interval_seconds": 1,
                "silent_warning_seconds": 10,
                "match": {
                    "vid": None,
                    "pid": None,
                    "serial_number": "",
                    "description_contains": "USB",
                },
                "startup_commands": ["PING", "STATUS"],
            },
            "power_faults": {
                "enabled": False,
                "backend": "windows_pnp",
                "mode": "random",
                "startup_delay_seconds": 1,
                "fixed_interval_seconds": 2,
                "random_min_seconds": 1,
                "random_max_seconds": 2,
                "off_duration_seconds": 1,
                "random_seed": None,
                "command_timeout_seconds": 1,
                "recovery_attempts": 1,
                "recovery_retry_seconds": 1,
                "max_cycles": None,
                "ensure_power_on_at_start": True,
                "ensure_power_on_on_exit": True,
                "windows_pnp": {"instance_id": "USB\\VID_0000&PID_0000\\REPLACE_ME"},
                "command": {"off": ["cmd", "/c", "echo", "off"], "on": ["cmd", "/c", "echo", "on"]},
            },
        }

        validate_config(config)


if __name__ == "__main__":
    unittest.main()
