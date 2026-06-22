from __future__ import annotations

import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from session_runner import SessionState, handle_protocol_event, handle_power_event


class SessionRunnerTests(unittest.TestCase):
    def test_handle_protocol_event_counts_boot_and_errors(self) -> None:
        state = SessionState()
        handle_protocol_event(state, {"event": "BOOT"})
        handle_protocol_event(state, {"event": "ERROR"})
        self.assertEqual(state.device_boot_events, 1)
        self.assertEqual(state.device_error_events, 1)

    def test_handle_protocol_event_tracks_failpoints(self) -> None:
        state = SessionState()
        handle_protocol_event(
            state,
            {"event": "FAILPOINT", "fields": {"name": "superblock_publish"}},
        )
        self.assertEqual(state.device_failpoint_events, 1)
        self.assertEqual(state.last_failpoint_name, "superblock_publish")
        self.assertIn("Failpoint reached: superblock_publish", state.notes)

    def test_handle_power_event_tracks_cycles(self) -> None:
        state = SessionState()
        events = []

        class Writer:
            def write(self, text: str) -> None:
                events.append(text)

            def flush(self) -> None:
                pass

        handle_power_event(
            state,
            {"event": "power_off_requested", "cycle": 1, "timestamp": "2024-01-01T00:00:00Z"},
            Writer(),
        )
        handle_power_event(
            state,
            {"event": "cycle_complete", "cycle": 1, "timestamp": "2024-01-01T00:00:00Z"},
            Writer(),
        )
        self.assertEqual(state.power_cycles_started, 1)
        self.assertEqual(state.power_cycles_completed, 1)


if __name__ == "__main__":
    unittest.main()
