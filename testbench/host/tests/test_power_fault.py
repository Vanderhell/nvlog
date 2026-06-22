from __future__ import annotations

import unittest
from types import SimpleNamespace
from pathlib import Path
import sys
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from power_fault import EventEmitter, create_controller, next_interval, run_command


class PowerFaultTests(unittest.TestCase):
    def test_next_interval_fixed(self) -> None:
        rng = SimpleNamespace(uniform=lambda a, b: 123.0)
        self.assertEqual(next_interval({"mode": "fixed", "fixed_interval_seconds": 2.5}, rng), 2.5)

    def test_next_interval_random(self) -> None:
        class DummyRandom:
            def uniform(self, a: float, b: float) -> float:
                return (a + b) / 2.0

        self.assertEqual(
            next_interval(
                {"mode": "random", "random_min_seconds": 1.0, "random_max_seconds": 3.0},
                DummyRandom(),
            ),
            2.0,
        )

    def test_create_controller_selects_windows_pnp(self) -> None:
        controller = create_controller(
            {
                "backend": "windows_pnp",
                "windows_pnp": {"instance_id": "USB\\VID_0000&PID_0000\\X"},
                "command_timeout_seconds": 1,
            },
            EventEmitter(),
        )
        self.assertEqual(controller.__class__.__name__, "WindowsPnpPowerController")

    @patch("power_fault.subprocess.run")
    def test_run_command_reports_failure(self, run_mock) -> None:
        run_mock.return_value = SimpleNamespace(returncode=5, stdout="failure output")
        with self.assertRaises(RuntimeError):
            run_command(["cmd", "/c", "exit", "5"], 1.0, EventEmitter())


if __name__ == "__main__":
    unittest.main()
