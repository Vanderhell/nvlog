# HWTestBench

Arduino-ESP32 bench project for the ESP32-S3 target.

## Target

- PlatformIO board id: `esp32-s3-devkitc-1`
- Board FQBN: `esp32:esp32:esp32s3`
- Flash mode: `qio`
- PSRAM mode: `QSPI PSRAM enabled`
- Expected PSRAM size: `8388608` bytes

## Reproducible build

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=default,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --output-dir build-arduino testbench\firmware\HWTestBench
```

## Reproducible upload

```powershell
arduino-cli upload -p COM19 --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=default,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --input-dir build-arduino testbench\firmware\HWTestBench
```

## Verified hardware result

The current validated hardware session passed end to end on the ESP32-S3 board:

- `PSRAM_TEST` reported `psram_bytes = 8388608`
- `PROJECT_TEST` executed and completed
- `ring_failpoint_smoke` passed
- `SESSION_PASS` was emitted
- `power_cycles_started = 100`
- `power_cycles_completed = 100`

Session artifacts are recorded under
`testbench/sessions-hw100/20260622_144401/`.
