# SerialSmoke

Minimal ESP32-S3 serial smoke target for proving post-flash RX before any bench
logic.

## Behavior

- Starts `Serial` at `115200`.
- Prints `HELLO_FROM_APP` after boot.
- Prints `APP_COUNTER` once per second.
- Replies to `PING` with `NVLOG|ACK|command=PING`.
- Replies to `STATUS` with `NVLOG|STATUS|mode=serial_minimal`.

## Build

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=default,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --output-dir build-serial-minimal testbench\firmware\SerialSmoke
```

## Upload

```powershell
arduino-cli upload -p COM19 --fqbn esp32:esp32:esp32s3:PSRAM=enabled,UploadMode=default,USBMode=default,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio --input-dir build-serial-minimal testbench\firmware\SerialSmoke
```

## Notes

This target intentionally avoids display, SD, PSRAM, nvlog, and project-adapter
initialization.
