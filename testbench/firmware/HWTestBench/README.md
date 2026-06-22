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

## Current local blocker

The current local validation run still needs serial RX re-confirmation after flash before `PSRAM_TEST`, `PROJECT_TEST`, and `hw100` can be trusted.
