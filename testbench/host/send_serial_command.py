from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Optional

import serial
from serial.tools import list_ports


def resolve_port(port: str, match_description: str) -> Optional[str]:
    configured = port.strip()
    if configured:
        return configured

    description = match_description.strip().lower()
    matches = []
    for item in list_ports.comports():
        if description and description not in str(item.description or "").lower():
            continue
        matches.append(item.device)

    if len(matches) == 1:
        return matches[0]
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Send a single serial command.")
    parser.add_argument("--port", default="", help="Explicit serial port to open.")
    parser.add_argument(
        "--match-description",
        default="",
        help="Match a serial port whose description contains this text.",
    )
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--command", required=True)
    parser.add_argument(
        "--linger",
        type=float,
        default=0.2,
        help="Seconds to wait after sending the command before closing.",
    )
    parser.add_argument(
        "--delay-before-send",
        type=float,
        default=0.0,
        help="Seconds to wait before opening the serial port.",
    )
    parser.add_argument(
        "--open-timeout",
        type=float,
        default=5.0,
        help="Seconds to keep retrying serial open before failing.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    port = resolve_port(args.port, args.match_description)
    if not port:
        print("Could not resolve a unique serial port.", file=sys.stderr)
        return 2

    try:
        if args.delay_before_send > 0:
            time.sleep(args.delay_before_send)
        deadline = time.time() + max(args.open_timeout, 0.0)
        connection = None
        while connection is None:
            try:
                connection = serial.Serial(
                    port, args.baudrate, timeout=args.timeout, write_timeout=2
                )
            except Exception as exc:
                if time.time() >= deadline:
                    raise exc
                time.sleep(0.1)
    except Exception as exc:
        print(f"Failed to open {port}: {exc}", file=sys.stderr)
        return 3

    try:
        payload = args.command.rstrip("\r\n") + "\n"
        connection.write(payload.encode("utf-8"))
        connection.flush()
        time.sleep(max(args.linger, 0.0))
    finally:
        connection.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
