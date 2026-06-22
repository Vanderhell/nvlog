from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Launch a delayed serial command.")
    parser.add_argument("--port", default="", help="Explicit serial port to target.")
    parser.add_argument(
        "--match-description",
        default="",
        help="Match a serial port whose description contains this text.",
    )
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--delay-before-send", type=float, default=1.0)
    parser.add_argument("--command", required=True)
    parser.add_argument(
        "--linger",
        type=float,
        default=0.2,
        help="Seconds to keep the port open after sending.",
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
    script = Path(__file__).resolve().with_name("send_serial_command.py")
    command = [
        sys.executable,
        str(script),
        "--port",
        args.port,
        "--match-description",
        args.match_description,
        "--baudrate",
        str(args.baudrate),
        "--delay-before-send",
        str(args.delay_before_send),
        "--linger",
        str(args.linger),
        "--open-timeout",
        str(args.open_timeout),
        "--command",
        args.command,
    ]
    subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
