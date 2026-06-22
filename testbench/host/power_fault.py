from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

from bench_common import (
    command_preview,
    fault_mechanism_from_backend,
    load_config,
    utc_now_iso,
    validate_config,
)


class StopController:
    def __init__(self) -> None:
        self.event = threading.Event()
        self.power_off_event = threading.Event()
        self.thread = threading.Thread(target=self._read_stdin, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _read_stdin(self) -> None:
        try:
            for line in sys.stdin:
                message = line.strip().upper()
                if message == "STOP":
                    self.event.set()
                    return
                if message == "POWER_OFF":
                    self.power_off_event.set()
        except Exception as exc:
            print(f"control stdin error: {exc!r}", file=sys.stderr, flush=True)
            self.event.set()
            self.power_off_event.set()

    def wait(self, seconds: float) -> bool:
        return self.event.wait(seconds)

    def wait_power_off(self, seconds: float) -> bool:
        return self.power_off_event.wait(seconds)


class EventEmitter:
    def emit(self, event: str, **fields: Any) -> None:
        payload = {
            "source": "power_fault",
            "timestamp": utc_now_iso(),
            "event": event,
        }
        payload.update(fields)
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True), flush=True)


class PowerController:
    def __init__(self, config: Dict[str, Any], emitter: EventEmitter) -> None:
        self.config = config
        self.emitter = emitter

    def power_off(self) -> None:
        raise NotImplementedError

    def power_on(self) -> None:
        raise NotImplementedError


class NoopPowerController(PowerController):
    def power_off(self) -> None:
        self.emitter.emit("noop_power_off")

    def power_on(self) -> None:
        self.emitter.emit("noop_power_on")


class WindowsPnpPowerController(PowerController):
    def __init__(self, config: Dict[str, Any], emitter: EventEmitter) -> None:
        super().__init__(config, emitter)
        self.instance_id = str(config["windows_pnp"]["instance_id"])
        self.timeout = float(config["command_timeout_seconds"])

    def _run(self, action: str) -> None:
        if action == "off":
            command = ["pnputil", "/disable-device", self.instance_id, "/force"]
        else:
            command = ["pnputil", "/enable-device", self.instance_id]
        run_command(command, self.timeout, self.emitter)

    def power_off(self) -> None:
        self._run("off")

    def power_on(self) -> None:
        self._run("on")


class CommandPowerController(PowerController):
    def __init__(self, config: Dict[str, Any], emitter: EventEmitter) -> None:
        super().__init__(config, emitter)
        command = config["command"]
        self.off_command = list(command["off"])
        self.on_command = list(command["on"])
        self.timeout = float(config["command_timeout_seconds"])

    def power_off(self) -> None:
        run_command(self.off_command, self.timeout, self.emitter)

    def power_on(self) -> None:
        run_command(self.on_command, self.timeout, self.emitter)


def run_command(command: List[str], timeout: float, emitter: EventEmitter) -> None:
    emitter.emit("command_start", command=command_preview(command))
    completed = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        shell=False,
        check=False,
    )
    output = completed.stdout.strip()
    emitter.emit(
        "command_result",
        command=command_preview(command),
        returncode=completed.returncode,
        output=output[-2000:],
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"Power command failed with exit code {completed.returncode}: {output}"
        )


def create_controller(config: Dict[str, Any], emitter: EventEmitter) -> PowerController:
    backend = config["backend"]
    if backend == "noop":
        return NoopPowerController(config, emitter)
    if backend == "windows_pnp":
        return WindowsPnpPowerController(config, emitter)
    if backend == "command":
        return CommandPowerController(config, emitter)
    raise ValueError(f"Unsupported backend: {backend}")


def next_interval(config: Dict[str, Any], rng: random.Random) -> float:
    if config["mode"] == "fixed":
        return float(config["fixed_interval_seconds"])
    return rng.uniform(
        float(config["random_min_seconds"]),
        float(config["random_max_seconds"]),
    )


def restore_power(
    controller: PowerController,
    config: Dict[str, Any],
    emitter: EventEmitter,
    stop: Optional[StopController] = None,
) -> bool:
    attempts = int(config["recovery_attempts"])
    retry_seconds = float(config["recovery_retry_seconds"])

    for attempt in range(1, attempts + 1):
        try:
            emitter.emit("power_on_requested", attempt=attempt, cleanup=stop is None)
            controller.power_on()
            emitter.emit("power_on", attempt=attempt, cleanup=stop is None)
            return True
        except Exception as exc:
            emitter.emit(
                "power_on_failed",
                attempt=attempt,
                cleanup=stop is None,
                error=repr(exc),
            )
            if attempt < attempts:
                if stop is None:
                    time.sleep(retry_seconds)
                else:
                    stop.wait(retry_seconds)
    return False


def run(config_path: Path) -> int:
    config = load_config(config_path)
    validate_config(config)
    faults = config["power_faults"]
    emitter = EventEmitter()

    if not faults["enabled"]:
        emitter.emit("disabled")
        return 0

    controller = create_controller(faults, emitter)
    stop = StopController()
    stop.start()

    seed = faults.get("random_seed")
    rng = random.Random(seed if isinstance(seed, int) else None)
    cycle = 0
    power_may_be_off = False
    max_cycles = faults.get("max_cycles")
    target_cycles = int(max_cycles) if isinstance(max_cycles, int) and max_cycles > 0 else None

    emitter.emit(
        "ready",
        backend=faults["backend"],
        mode=faults["mode"],
        random_seed=seed,
        max_cycles=target_cycles,
        mechanism=fault_mechanism_from_backend(str(faults["backend"])),
    )

    try:
        if bool(faults.get("ensure_power_on_at_start", True)):
            if not restore_power(controller, faults, emitter, stop):
                emitter.emit("fatal", error="Unable to establish powered-on state.")
                return 3

        startup_delay = float(faults["startup_delay_seconds"])
        emitter.emit("startup_delay", seconds=startup_delay)
        if stop.wait(startup_delay):
            return 0

        while not stop.event.is_set():
            if target_cycles is not None and cycle >= target_cycles:
                emitter.emit("cycle_limit_reached", completed_cycles=cycle)
                break
            interval = next_interval(faults, rng)
            emitter.emit("next_fault_scheduled", cycle=cycle + 1, seconds=interval)
            if stop.wait(interval):
                break

            cycle += 1
            emitter.emit("power_off_requested", cycle=cycle)
            try:
                if stop is not None:
                    ack_timeout = max(float(faults["command_timeout_seconds"]), 2.0)
                    if not stop.wait_power_off(ack_timeout):
                        raise RuntimeError("Timed out waiting for host power-off ack.")
                controller.power_off()
                power_may_be_off = True
                emitter.emit("power_off", cycle=cycle)
            except Exception as exc:
                emitter.emit("power_off_failed", cycle=cycle, error=repr(exc))
                continue

            off_seconds = float(faults["off_duration_seconds"])
            emitter.emit("power_off_hold", cycle=cycle, seconds=off_seconds)
            stop.wait(off_seconds)

            if not restore_power(controller, faults, emitter, stop):
                emitter.emit(
                    "fatal",
                    cycle=cycle,
                    error="Unable to restore power after configured attempts.",
                )
                return 4
            power_may_be_off = False
            emitter.emit("cycle_complete", cycle=cycle)

        return 0
    finally:
        if power_may_be_off or bool(faults.get("ensure_power_on_on_exit", True)):
            restored = restore_power(controller, faults, emitter, None)
            if not restored:
                emitter.emit(
                    "cleanup_failed",
                    error="Final power-on recovery failed. Check the target manually.",
                )
        emitter.emit("stopped", completed_cycles=cycle)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate fixed or random USB/power interruption cycles."
    )
    parser.add_argument(
        "--config",
        type=Path,
        required=True,
        help="Path to the shared testbench JSON configuration.",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Validate configuration and exit without touching hardware.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        config = load_config(args.config)
        validate_config(config)
        if args.validate:
            print("Configuration is valid.")
            return 0
        return run(args.config)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"power_fault fatal error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
