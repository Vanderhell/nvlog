# Hardware Validation

This document records the NVLog hardware bench target and the current
validation status for the ESP32-S3 test device.

## Target configuration

- PlatformIO board id: `esp32-s3-devkitc-1`
- Board FQBN: `esp32:esp32:esp32s3`
- Flash mode: `qio`
- PSRAM mode: `QSPI PSRAM enabled`
- Expected PSRAM size: `8388608` bytes

## Reproducible Arduino CLI commands

Build:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=default,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --output-dir build-arduino testbench\firmware\HWTestBench
```

Upload:

```powershell
arduino-cli upload -p COM19 --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=default,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --input-dir build-arduino testbench\firmware\HWTestBench
```

## Current validation state

- The board enumerates as `USB Serial Device (COM19)`.
- The host opens `COM19`.
- The validated session completed with `SESSION_PASS`.
- `power_cycles_started = 100`.
- `power_cycles_completed = 100`.
- `device_failpoint_events = 3`.
- `last_failpoint_name = superblock_publish`.
- `ring_failpoint_smoke` passed.
- `psram_api_smoke` passed and reported `psram_bytes = 8388608`.
- Host-side tests passed: `11 passed`.

Session artifacts:

- `testbench/sessions-hw100/20260622_144401/summary.json`
- `testbench/sessions-hw100/20260622_144401/events.jsonl`
- `testbench/sessions-hw100/20260622_144401/serial.log`
- `testbench/sessions-hw100/20260622_144401/effective_config.json`
