# Hardware Validation

Public validation summary for the NVLog hardware bench.

Validated on `2026-06-21` with the ESP32-S3 HW bench:

- `arduino-cli compile` for `testbench/firmware/HWTestBench` passed with the ESP32-S3 PSRAM board profile.
- Host-side bench runner and power-fault worker unit tests passed.
- The bench firmware reports live progress on the display.
- SDMMC logging through `SD_MMC` is wired and exercised by the bench.
- PSRAM capability detection is present on the ESP32-S3 N16R8 board.
- The hardware suite includes a 100-cycle power-cut stress configuration.

This document records the stable validation outcome only.
