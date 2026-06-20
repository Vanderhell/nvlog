# Changelog

## Unreleased

- Pending follow-up work will continue under the next audited release line.

## 1.0.1 - 2026-06-19

- Updated the public append API to accept `size_t` lengths so `NVLOG_ERR_TOO_LARGE` is reachable before narrowing.
- Added ring tests for `UINT16_MAX`, `UINT32_MAX`, and `SIZE_MAX`-adjacent length validation.
- Removed the stale `NVLOG_RECORD_TYPE_WRAP_PAD` alias from the public record-type enum.
- Rewrote the v0.5 media-format document to match the current encoded superblock and record layout.
- Preserved flash geometry metadata through `nvlog_format()` and `nvlog_mount()` so the flash helper's public fields remain live after initialization.
- Added flash-geometry assertions to the host flash backend test.
- Added host coverage for flash program-unit geometry values 1, 4, 8, and 32.
- Removed stale README status claims that overstated verification.
- Normalized public version/status text in headers and test banners.
- Fixed the currently reported compiler warnings in the host build.
- Added compile-only coverage for the FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, and ESP-IDF partition example backends.
- Added public-header compile checks for both C and C++ consumers.
- Updated README evidence language to distinguish host-tested, simulator-tested, and compile-verified items from unverified hardware behavior.

## 1.0.0 - 2026-03-10

- Historical public release baseline preserved for the tagged `v1.0.0` commit.
