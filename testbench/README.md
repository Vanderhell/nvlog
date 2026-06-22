# NVLog Embedded Testbench

This tree contains the Arduino firmware, Python host runner, power-fault
controller, protocol notes, and configuration used to exercise NVLog on an
ESP32-S3 board with the 0.96-inch display.

## Layout

- `host/` - Python session runner, fault process, shared helpers, and tests.
- `firmware/HWTestBench/` - Arduino project for the board. See
  `firmware/HWTestBench/README.md` for the exact board id, FQBN, and build /
  upload commands.
- `docs/PROTOCOL.md` - serial event and command notes.
- `test_config.json` - default session configuration.

## Serial protocol

Firmware emits one-line machine-readable events prefixed with `NVLOG|`.
The host parser also accepts the legacy `@HWTEST|` form.

Examples:

```text
NVLOG|BOOT|boot=2
NVLOG|READY
NVLOG|SCENARIO_START|id=single_append
NVLOG|PHASE|name=recovery
NVLOG|SCENARIO_PASS|id=single_append
NVLOG|SCENARIO_FAIL|id=single_append|code=READ_MISMATCH
NVLOG|SESSION_PASS
NVLOG|SESSION_FAIL|failed=1
```

## Build

Install the Arduino CLI ESP32 core, then compile the firmware from
`firmware/HWTestBench` using the bundled `platformio.ini` or an equivalent
Arduino-ESP32 build setup.

If the board is running the app but upload does not auto-enter the bootloader,
send the `BOOTLOADER` command over the active serial port first and then start
the upload immediately after the board re-enumerates.

For a hardware stress validation run with 100 restart cycles, use
`test_config.hw100.json`. That config keeps the board on-screen updated with
the current scenario, host cycle counter, and last progress label.

The latest recorded `hw100` session completed successfully:

- `SESSION_PASS`
- `power_cycles_started = 100`
- `power_cycles_completed = 100`
- `device_failpoint_events = 3`
- `last_failpoint_name = superblock_publish`
- `ring_failpoint_smoke` passed
- `psram_api_smoke` passed with `psram_bytes = 8388608`

The session artifacts are in
`testbench/sessions-hw100/20260622_144401/`.

## Host run

The default session config points the runner at `COM19`.
Start the session runner with the shared JSON config and let it reconnect as
the device reboots or disconnects.

## Fault mechanism

The Python fault process reports the effective mechanism as:

- `USB_LOGICAL_DISCONNECT` for Windows PnP disable/enable
- `USB_POWER_CUT` for a real external power-switch command backend

The default configuration uses logical disconnect behavior.

## TF card

TF support remains configurable and is disabled by default until the exact
board wiring is confirmed.
