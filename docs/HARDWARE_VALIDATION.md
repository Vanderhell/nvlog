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

- The board has been confirmed to enumerate as `USB Serial Device (COM19)`.
- The host can open `COM19`.
- The latest local session did not capture device RX after flash.
- `PSRAM_TEST`, `PROJECT_TEST`, and `hw100` are not re-proven in the latest run.

This file intentionally avoids claiming completed validation until the serial RX
and scenario evidence are present in the session artifacts.
