from __future__ import annotations

import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, Optional


PROTOCOL_PREFIXES = ("NVLOG|", "@HWTEST|")


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def local_timestamp_for_path() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def load_config(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError("Configuration root must be a JSON object.")
    return data


def require_dict(parent: Dict[str, Any], key: str) -> Dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise ValueError(f"Configuration section '{key}' must be an object.")
    return value


def require_number(section: Dict[str, Any], key: str, minimum: float = 0.0) -> float:
    value = section.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"'{key}' must be a number.")
    if value < minimum:
        raise ValueError(f"'{key}' must be >= {minimum}.")
    return float(value)


def require_integer(section: Dict[str, Any], key: str, minimum: int = 0) -> int:
    value = section.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"'{key}' must be an integer.")
    if value < minimum:
        raise ValueError(f"'{key}' must be >= {minimum}.")
    return value


def require_optional_integer(section: Dict[str, Any], key: str, minimum: int = 0) -> Optional[int]:
    value = section.get(key)
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"'{key}' must be an integer or null.")
    if value < minimum:
        raise ValueError(f"'{key}' must be >= {minimum}.")
    return value


def validate_config(config: Dict[str, Any]) -> None:
    session = require_dict(config, "session")
    serial = require_dict(config, "serial")
    faults = require_dict(config, "power_faults")

    require_number(session, "duration_seconds", 1.0)
    require_number(session, "startup_settle_seconds", 0.0)
    require_number(session, "power_worker_stop_timeout_seconds", 1.0)

    baudrate = require_integer(serial, "baudrate", 1)
    if baudrate > 10_000_000:
        raise ValueError("'baudrate' is implausibly high.")
    require_number(serial, "read_timeout_seconds", 0.01)
    require_number(serial, "reconnect_interval_seconds", 0.05)
    require_number(serial, "silent_warning_seconds", 0.0)

    port = serial.get("port", "")
    if not isinstance(port, str):
        raise ValueError("'serial.port' must be a string.")

    match = serial.get("match", {})
    if not isinstance(match, dict):
        raise ValueError("'serial.match' must be an object.")

    startup_commands = serial.get("startup_commands", [])
    if not isinstance(startup_commands, list) or not all(
        isinstance(item, str) for item in startup_commands
    ):
        raise ValueError("'serial.startup_commands' must be a string array.")

    enabled = faults.get("enabled")
    if not isinstance(enabled, bool):
        raise ValueError("'power_faults.enabled' must be boolean.")

    backend = faults.get("backend")
    if backend not in {"noop", "windows_pnp", "command"}:
        raise ValueError(
            "'power_faults.backend' must be noop, windows_pnp, or command."
        )

    mode = faults.get("mode")
    if mode not in {"fixed", "random"}:
        raise ValueError("'power_faults.mode' must be fixed or random.")

    require_number(faults, "startup_delay_seconds", 0.0)
    require_number(faults, "fixed_interval_seconds", 0.1)
    random_min = require_number(faults, "random_min_seconds", 0.1)
    random_max = require_number(faults, "random_max_seconds", 0.1)
    if random_max < random_min:
        raise ValueError(
            "'power_faults.random_max_seconds' must be >= random_min_seconds."
        )
    require_number(faults, "off_duration_seconds", 0.1)
    require_number(faults, "command_timeout_seconds", 0.1)
    require_number(faults, "recovery_retry_seconds", 0.1)
    require_integer(faults, "recovery_attempts", 1)
    require_optional_integer(faults, "max_cycles", 1)

    if enabled and backend == "windows_pnp":
        pnp = faults.get("windows_pnp")
        if not isinstance(pnp, dict) or not str(pnp.get("instance_id", "")).strip():
            raise ValueError(
                "windows_pnp backend requires power_faults.windows_pnp.instance_id."
            )

    if enabled and backend == "command":
        command = faults.get("command")
        if not isinstance(command, dict):
            raise ValueError("command backend requires power_faults.command object.")
        for key in ("off", "on"):
            value = command.get(key)
            if not isinstance(value, list) or not value or not all(
                isinstance(item, str) and item for item in value
            ):
                raise ValueError(
                    f"power_faults.command.{key} must be a non-empty string array."
                )


def atomic_write_json(path: Path, value: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_name, path)
    except Exception:
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def append_jsonl(handle: Any, value: Dict[str, Any]) -> None:
    handle.write(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")
    handle.flush()


def parse_protocol_line(line: str) -> Optional[Dict[str, Any]]:
    prefix = next((item for item in PROTOCOL_PREFIXES if line.startswith(item)), None)
    if prefix is None:
        return None

    parts = line[len(prefix) :].split("|")
    if not parts or not parts[0]:
        return {"event": "MALFORMED", "raw": line}

    parsed: Dict[str, Any] = {"event": parts[0]}
    fields: Dict[str, str] = {}
    bare: list[str] = []

    for token in parts[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
        elif token:
            bare.append(token)

    if fields:
        parsed["fields"] = fields
    if bare:
        parsed["tokens"] = bare
    return parsed


def command_preview(command: Iterable[str]) -> str:
    return " ".join(repr(part) if " " in part else part for part in command)


def fault_mechanism_from_backend(backend: str) -> str:
    if backend == "command":
        return "USB_POWER_CUT"
    return "USB_LOGICAL_DISCONNECT"
