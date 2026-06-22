from __future__ import annotations

import argparse
import time

import serial


def capture_sequence(port_name: str, initial_dtr: bool, initial_rts: bool) -> dict[str, object]:
    result: dict[str, object] = {
        "port": port_name,
        "initial_dtr": initial_dtr,
        "initial_rts": initial_rts,
        "captured_lines": [],
        "rom_downloader_seen": False,
        "app_output_seen": False,
        "ping_ack_seen": False,
    }

    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 0.2
    port.dsrdtr = False
    port.rtscts = False
    port.dtr = initial_dtr
    port.rts = initial_rts
    port.open()
    try:
        time.sleep(0.3)
        port.dtr = False
        port.rts = True
        time.sleep(0.3)
        port.dtr = False
        port.rts = False
        time.sleep(0.3)
        port.reset_input_buffer()

        start = time.monotonic()
        sent_ping = False
        lines: list[str] = result["captured_lines"]  # type: ignore[assignment]

        while time.monotonic() - start < 15.0:
            elapsed = time.monotonic() - start
            if not sent_ping and elapsed >= 10.0:
                port.write(b"PING\n")
                port.flush()
                sent_ping = True
                print("TX PING")

            raw = port.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            lines.append(line)
            print("RX", line)
            if line.startswith("ESP-ROM:esp32s3-20210327") or "waiting for download" in line:
                result["rom_downloader_seen"] = True
            if (
                "HELLO_FROM_APP" in line
                or "APP_COUNTER" in line
                or "NVLOG|STATUS|mode=serial_minimal" in line
            ):
                result["app_output_seen"] = True
            if "NVLOG|ACK|command=PING" in line:
                result["ping_ack_seen"] = True
    finally:
        port.close()

    return result


def print_result(label: str, result: dict[str, object]) -> None:
    print(label)
    print(f"port={result['port']}")
    print(f"DTR={result['initial_dtr']}")
    print(f"RTS={result['initial_rts']}")
    print(f"ROM_DOWNLOADER_SEEN={result['rom_downloader_seen']}")
    print(f"APP_OUTPUT_SEEN={result['app_output_seen']}")
    print(f"PING_ACK_SEEN={result['ping_ack_seen']}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Probe ESP32-S3 app RX on COM19.")
    parser.add_argument("--port", default="COM19")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    first = capture_sequence(args.port, False, False)
    print_result("SEQUENCE_1", first)

    if not bool(first["app_output_seen"]) and not bool(first["ping_ack_seen"]):
        second = capture_sequence(args.port, True, False)
        print_result("SEQUENCE_2", second)
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
