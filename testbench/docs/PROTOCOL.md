# Serial Protocol

Firmware sends one machine-readable event per line:

```text
NVLOG|EVENT|key=value|key=value
```

`|`, CR, and LF are sanitized before emission. The host keeps the raw serial
log in addition to the structured JSON events.

The host parser also accepts the legacy `@HWTEST|` prefix.

## Common events

- `BOOT` - device boot and reset reason.
- `READY` - firmware finished startup and is ready for the session.
- `HEARTBEAT` - periodic liveness event.
- `STATUS` - current benchmark and peripheral state.
- `SCENARIO_START` - scenario is beginning.
- `PHASE` - scenario phase change.
- `SCENARIO_PASS` - scenario passed.
- `SCENARIO_FAIL` - scenario failed.
- `SCENARIO_SKIPPED` - scenario skipped with a reason.
- `SESSION_PASS` - all required scenarios finished successfully.
- `SESSION_FAIL` - one or more scenarios failed.
- `ERROR` - protocol or command error.

## Host commands

```text
PING
STATUS
DISPLAY_TEST
DISPLAY <text>
SD_TEST
SD_APPEND <text>
SD_READ
PSRAM_TEST
PROJECT_TEST
SESSION_PROGRESS <fields>
REBOOT
BOOTLOADER
HELP
```

The project adapter may accept extra commands if needed.
