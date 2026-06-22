# SerialSmoke

Minimal ESP32-S3 serial smoke target for proving post-flash RX before any bench
logic.

## Behavior

- Uses `HWCDC` when building for ESP32-S3 USB-Serial/JTAG mode, otherwise
  falls back to `Serial`, both at `115200`.
- Prints `HELLO_FROM_APP` after boot.
- Prints `APP_COUNTER` once per second.
- Replies to `PING` with `NVLOG|ACK|command=PING`.
- Replies to `STATUS` with `NVLOG|STATUS|mode=serial_minimal`.

## Build

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=cdc,USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --output-dir build-serial-minimal-hwcdc testbench\firmware\SerialSmoke
```

## Upload

```powershell
arduino-cli upload -p COM19 --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=cdc,USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --input-dir build-serial-minimal-hwcdc testbench\firmware\SerialSmoke
```

## Notes

This target intentionally avoids display, SD, PSRAM, nvlog, and project-adapter
initialization.
