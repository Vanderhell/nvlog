from __future__ import annotations

import argparse
import json
import queue
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, Optional

import serial
from serial import SerialException
from serial.tools import list_ports

from bench_common import (
    append_jsonl,
    atomic_write_json,
    fault_mechanism_from_backend,
    load_config,
    local_timestamp_for_path,
    parse_protocol_line,
    utc_now_iso,
    validate_config,
)


class SessionState:
    def __init__(self) -> None:
        self.started_at = utc_now_iso()
        self.finished_at: Optional[str] = None
        self.stop_requested = False
        self.serial_connect_count = 0
        self.serial_disconnect_count = 0
        self.serial_lines_received = 0
        self.serial_decode_replacements = 0
        self.device_boot_events = 0
        self.device_heartbeats = 0
        self.device_error_events = 0
        self.device_failpoint_events = 0
        self.last_failpoint_name: Optional[str] = None
        self.power_cycles_started = 0
        self.power_cycles_completed = 0
        self.power_worker_failures = 0
        self.fault_mechanism: Optional[str] = None
        self.last_protocol_event: Optional[Dict[str, Any]] = None
        self.notes: list[str] = []

    def to_dict(self) -> Dict[str, Any]:
        return dict(self.__dict__)


class PowerWorker:
    def __init__(
        self,
        script_path: Path,
        config_path: Path,
        events: "queue.Queue[Dict[str, Any]]",
    ) -> None:
        self.events = events
        self.process = subprocess.Popen(
            [sys.executable, "-u", str(script_path), "--config", str(config_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self.reader = threading.Thread(target=self._read_output, daemon=True)
        self.reader.start()

    def _read_output(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            text = line.strip()
            if not text:
                continue
            try:
                payload = json.loads(text)
                if isinstance(payload, dict):
                    self.events.put(payload)
                    continue
            except json.JSONDecodeError:
                pass
            self.events.put(
                {
                    "source": "power_fault",
                    "timestamp": utc_now_iso(),
                    "event": "worker_output",
                    "text": text,
                }
            )

    def poll(self) -> Optional[int]:
        return self.process.poll()

    def stop(self, timeout_seconds: float) -> int:
        if self.process.poll() is not None:
            return int(self.process.returncode or 0)

        try:
            if self.process.stdin is not None:
                self.process.stdin.write("STOP\n")
                self.process.stdin.flush()
            return self.process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                return self.process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                return self.process.wait(timeout=5.0)

    def signal_power_off(self) -> None:
        if self.process.poll() is not None or self.process.stdin is None:
            return
        self.process.stdin.write("POWER_OFF\n")
        self.process.stdin.flush()


def port_matches(port: Any, serial_config: Dict[str, Any]) -> bool:
    match = serial_config.get("match", {})
    if not match:
        return False

    vid = match.get("vid")
    pid = match.get("pid")
    serial_number = str(match.get("serial_number", "")).strip().lower()
    description = str(match.get("description_contains", "")).strip().lower()

    if vid is not None and port.vid != int(vid):
        return False
    if pid is not None and port.pid != int(pid):
        return False
    if serial_number and serial_number not in str(port.serial_number or "").lower():
        return False
    if description and description not in str(port.description or "").lower():
        return False
    return any(value not in (None, "") for value in (vid, pid, serial_number, description))


def resolve_port(serial_config: Dict[str, Any]) -> Optional[str]:
    configured = str(serial_config.get("port", "")).strip()
    ports = list(list_ports.comports())

    if configured:
        for port in ports:
            if port.device.lower() == configured.lower():
                return port.device
        if bool(serial_config.get("strict_configured_port", False)):
            return None

    matches = [port.device for port in ports if port_matches(port, serial_config)]
    if len(matches) == 1:
        return matches[0]
    return None


def print_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return

    for port in ports:
        vid = f"0x{port.vid:04X}" if port.vid is not None else "-"
        pid = f"0x{port.pid:04X}" if port.pid is not None else "-"
        print(
            f"{port.device}: {port.description} | VID={vid} PID={pid} "
            f"SERIAL={port.serial_number or '-'} | HWID={port.hwid}"
        )


def write_serial_line(
    raw_handle: Any,
    direction: str,
    line: str,
    session_start_monotonic: float,
) -> None:
    elapsed = time.monotonic() - session_start_monotonic
    raw_handle.write(
        f"{utc_now_iso()} +{elapsed:012.3f}s {direction} {line.rstrip()}\n"
    )
    raw_handle.flush()


def send_command(
    connection: serial.Serial,
    raw_handle: Any,
    command: str,
    session_start_monotonic: float,
) -> None:
    payload = (command.rstrip("\r\n") + "\n").encode("utf-8")
    connection.write(payload)
    connection.flush()
    write_serial_line(raw_handle, "TX", command, session_start_monotonic)


def send_progress(
    connection: serial.Serial,
    raw_handle: Any,
    session_start_monotonic: float,
    completed_cycles: int,
    target_cycles: Optional[int],
    label: str,
) -> None:
    total = target_cycles if target_cycles is not None else 0
    command = f"SESSION_PROGRESS cycle={completed_cycles} target={total} label={label}"
    send_command(connection, raw_handle, command, session_start_monotonic)


def handle_protocol_event(state: SessionState, parsed: Dict[str, Any]) -> None:
    state.last_protocol_event = parsed
    event = str(parsed.get("event", ""))
    if event == "BOOT":
        state.device_boot_events += 1
    elif event == "HEARTBEAT":
        state.device_heartbeats += 1
    elif event in {"ERROR", "MALFORMED"}:
        state.device_error_events += 1
    elif event == "FAILPOINT":
        state.device_failpoint_events += 1
        fields = parsed.get("fields", {})
        if isinstance(fields, dict):
            name = fields.get("name")
            if isinstance(name, str) and name:
                state.last_failpoint_name = name
                state.notes.append(f"Failpoint reached: {name}")


def handle_power_event(
    state: SessionState,
    payload: Dict[str, Any],
    events_handle: Any,
) -> bool:
    append_jsonl(events_handle, payload)
    event = str(payload.get("event", ""))
    if event == "power_off_requested":
        state.power_cycles_started += 1
        return True
    if event == "power_off":
        return False
    if event == "cycle_complete":
        state.power_cycles_completed += 1
    if event == "cycle_limit_reached":
        state.notes.append(f"Power worker reached configured cycle limit at {payload.get('completed_cycles')}.")
    if event in {"fatal", "cleanup_failed"}:
        state.power_worker_failures += 1
    return False


def run_session(config_path: Path) -> int:
    config = load_config(config_path)
    validate_config(config)
    session_config = config["session"]
    serial_config = config["serial"]
    fault_config = config["power_faults"]
    state = SessionState()
    state.fault_mechanism = fault_mechanism_from_backend(str(fault_config["backend"]))

    project_root = Path(__file__).resolve().parent.parent
    output_root = Path(session_config.get("output_directory", "sessions"))
    if not output_root.is_absolute():
        output_root = project_root / output_root

    session_dir = output_root / local_timestamp_for_path()
    session_dir.mkdir(parents=True, exist_ok=False)

    raw_path = session_dir / "serial.log"
    events_path = session_dir / "events.jsonl"
    summary_path = session_dir / "summary.json"
    config_copy_path = session_dir / "effective_config.json"
    atomic_write_json(config_copy_path, config)

    stop_event = threading.Event()
    power_events: "queue.Queue[Dict[str, Any]]" = queue.Queue()
    power_worker: Optional[PowerWorker] = None
    connection: Optional[serial.Serial] = None
    connected_port: Optional[str] = None
    session_start = time.monotonic()
    deadline = session_start + float(session_config["duration_seconds"])
    last_rx = session_start
    last_silent_warning = 0.0
    reconnect_at = session_start
    worker_exit_reported = False
    power_cycle_pending = False
    target_cycles = fault_config.get("max_cycles")
    power_cycle_target = int(target_cycles) if isinstance(target_cycles, int) and target_cycles > 0 else None

    def request_stop(signum: int, frame: Any) -> None:
        del signum, frame
        state.stop_requested = True
        stop_event.set()

    signal.signal(signal.SIGINT, request_stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, request_stop)

    print(f"Session directory: {session_dir}")
    print(
        f"Duration: {float(session_config['duration_seconds']):.1f} s | "
        f"Serial: {serial_config.get('port') or 'auto-match'} @ "
        f"{serial_config['baudrate']} baud | "
        f"Mechanism: {state.fault_mechanism}"
    )

    with raw_path.open("w", encoding="utf-8", newline="\n") as raw_handle, events_path.open(
        "w", encoding="utf-8", newline="\n"
    ) as events_handle:
        append_jsonl(
            events_handle,
            {
                "source": "session_runner",
                "timestamp": utc_now_iso(),
                "event": "session_started",
                "session_directory": str(session_dir),
            },
        )

        try:
            if fault_config["enabled"]:
                power_worker = PowerWorker(
                    Path(__file__).resolve().parent / "power_fault.py",
                    config_path.resolve(),
                    power_events,
                )

            while not stop_event.is_set() and time.monotonic() < deadline:
                close_serial_requested = False
                while True:
                    try:
                        power_payload = power_events.get_nowait()
                    except queue.Empty:
                        break
                    close_serial_requested |= handle_power_event(
                        state, power_payload, events_handle
                    )
                    event_name = str(power_payload.get("event", ""))
                    if event_name == "power_off_requested":
                        power_cycle_pending = True
                    elif event_name == "power_off":
                        power_cycle_pending = False
                        reconnect_at = time.monotonic() + float(
                            serial_config["reconnect_interval_seconds"]
                        )
                    elif event_name in {"power_off_failed", "cycle_complete"}:
                        power_cycle_pending = False
                        reconnect_at = time.monotonic() + float(
                            serial_config["reconnect_interval_seconds"]
                        )
                    print(
                        f"[power] {power_payload.get('event')}: "
                        f"{power_payload.get('cycle', '')}"
                    )

                if close_serial_requested and connection is not None:
                    connection.close()
                    connection = None
                    connected_port = None
                    state.serial_disconnect_count += 1
                    append_jsonl(
                        events_handle,
                        {
                            "source": "session_runner",
                            "timestamp": utc_now_iso(),
                            "event": "serial_closed_for_power_fault",
                        },
                    )
                if close_serial_requested and power_worker is not None:
                    power_worker.signal_power_off()

                if power_worker is not None and not worker_exit_reported:
                    returncode = power_worker.poll()
                    if returncode is not None:
                        worker_exit_reported = True
                        append_jsonl(
                            events_handle,
                            {
                                "source": "session_runner",
                                "timestamp": utc_now_iso(),
                                "event": "power_worker_exited",
                                "returncode": returncode,
                            },
                        )
                        if returncode != 0:
                            state.power_worker_failures += 1
                            if bool(session_config.get("abort_on_power_worker_failure", True)):
                                state.notes.append(
                                    f"Power worker exited with code {returncode}."
                                )
                                stop_event.set()
                                continue
                        elif power_cycle_target is not None and state.power_cycles_completed >= power_cycle_target:
                            state.notes.append(
                                f"Power worker completed configured cycle limit of {power_cycle_target}."
                            )
                            stop_event.set()
                            continue

                now = time.monotonic()
                if connection is None and not power_cycle_pending and now >= reconnect_at:
                    port = resolve_port(serial_config)
                    if port is None:
                        reconnect_at = now + float(
                            serial_config["reconnect_interval_seconds"]
                        )
                    else:
                        try:
                            connection = serial.Serial(
                                port=port,
                                baudrate=int(serial_config["baudrate"]),
                                timeout=float(serial_config["read_timeout_seconds"]),
                                write_timeout=float(
                                    serial_config.get("write_timeout_seconds", 2.0)
                                ),
                                exclusive=None,
                            )
                            connected_port = port
                            state.serial_connect_count += 1
                            last_rx = time.monotonic()
                            append_jsonl(
                                events_handle,
                                {
                                    "source": "session_runner",
                                    "timestamp": utc_now_iso(),
                                    "event": "serial_connected",
                                    "port": port,
                                },
                            )
                            print(f"[serial] connected: {port}")

                            settle = float(session_config["startup_settle_seconds"])
                            if settle > 0:
                                stop_event.wait(settle)
                            send_progress(
                                connection,
                                raw_handle,
                                session_start,
                                state.power_cycles_completed,
                                power_cycle_target,
                                "connected",
                            )
                            mode_command = "SESSION_MODE faults={mode} mechanism={mech}".format(
                                mode="enabled" if bool(fault_config["enabled"]) else "disabled",
                                mech=state.fault_mechanism or "USB_LOGICAL_DISCONNECT",
                            )
                            send_command(connection, raw_handle, mode_command, session_start)
                            for command in serial_config.get("startup_commands", []):
                                send_command(
                                    connection, raw_handle, command, session_start
                                )
                        except (OSError, SerialException) as exc:
                            connection = None
                            connected_port = None
                            reconnect_at = time.monotonic() + float(
                                serial_config["reconnect_interval_seconds"]
                            )
                            append_jsonl(
                                events_handle,
                                {
                                    "source": "session_runner",
                                    "timestamp": utc_now_iso(),
                                    "event": "serial_connect_failed",
                                    "port": port,
                                    "error": repr(exc),
                                },
                            )

                if connection is None:
                    stop_event.wait(0.05)
                    continue

                try:
                    raw = connection.readline()
                    if raw:
                        decoded = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                        if "\ufffd" in decoded:
                            state.serial_decode_replacements += decoded.count("\ufffd")
                        state.serial_lines_received += 1
                        last_rx = time.monotonic()
                        write_serial_line(raw_handle, "RX", decoded, session_start)
                        parsed = parse_protocol_line(decoded)
                        if parsed is not None:
                            handle_protocol_event(state, parsed)
                            append_jsonl(
                                events_handle,
                                {
                                    "source": "device",
                                    "timestamp": utc_now_iso(),
                                    "event": "protocol_event",
                                    "port": connected_port,
                                    "protocol": parsed,
                                },
                            )
                            print(f"[device] {parsed.get('event')}: {parsed.get('fields', {})}")
                    else:
                        silent_seconds = float(serial_config["silent_warning_seconds"])
                        now = time.monotonic()
                        if (
                            silent_seconds > 0
                            and now - last_rx >= silent_seconds
                            and now - last_silent_warning >= silent_seconds
                        ):
                            last_silent_warning = now
                            append_jsonl(
                                events_handle,
                                {
                                    "source": "session_runner",
                                    "timestamp": utc_now_iso(),
                                    "event": "serial_silent",
                                    "seconds": now - last_rx,
                                    "port": connected_port,
                                },
                            )
                except (OSError, SerialException) as exc:
                    append_jsonl(
                        events_handle,
                        {
                            "source": "session_runner",
                            "timestamp": utc_now_iso(),
                            "event": "serial_disconnected",
                            "port": connected_port,
                            "error": repr(exc),
                        },
                    )
                    print(f"[serial] disconnected: {exc}")
                    try:
                        connection.close()
                    except Exception:
                        pass
                    connection = None
                    connected_port = None
                    state.serial_disconnect_count += 1
                    reconnect_at = time.monotonic() + float(
                        serial_config["reconnect_interval_seconds"]
                    )

        finally:
            if connection is not None:
                try:
                    connection.close()
                except Exception:
                    pass

            if power_worker is not None:
                returncode = power_worker.stop(
                    float(session_config["power_worker_stop_timeout_seconds"])
                )
                append_jsonl(
                    events_handle,
                    {
                        "source": "session_runner",
                        "timestamp": utc_now_iso(),
                        "event": "power_worker_stopped",
                        "returncode": returncode,
                    },
                )
                if returncode != 0:
                    state.power_worker_failures += 1

            while True:
                try:
                    payload = power_events.get_nowait()
                except queue.Empty:
                    break
                handle_power_event(state, payload, events_handle)

            state.finished_at = utc_now_iso()
            append_jsonl(
                events_handle,
                {
                    "source": "session_runner",
                    "timestamp": state.finished_at,
                    "event": "session_finished",
                },
            )
            atomic_write_json(summary_path, state.to_dict())

    print(f"Summary: {summary_path}")
    if state.power_worker_failures > 0:
        return 3
    if state.device_error_events > 0:
        return 4
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Monitor an embedded serial port and coordinate power faults."
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=Path("test_config.json"),
        help="Shared JSON configuration file.",
    )
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List available serial ports and exit.",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Validate configuration and exit.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.list_ports:
        print_ports()
        return 0

    try:
        config = load_config(args.config)
        validate_config(config)
        if args.validate:
            print("Configuration is valid.")
            return 0
        return run_session(args.config)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"session_runner fatal error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
