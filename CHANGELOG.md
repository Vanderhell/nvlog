# Changelog

## 1.0.5 - 2026-06-20

- Final audited release head after the stage evidence, CI, and release-tag audit pass.
- Recorded the completed stage ledger, final audit evidence, and generated evidence manifest.
- Anchored the audited head with a fresh annotated release tag.

## 1.0.4 - 2026-06-20

- Final audited release head after the stage evidence and release-tag audit pass.
- Recorded the completed stage ledger and final audit evidence.
- Anchored the audited head with a fresh annotated release tag.

## 1.0.3 - 2026-06-20

- Finalized the audit ledger and release-evidence records.
- Locked the audited release head to the repository state that passed the full local suite.

## 1.0.2 - 2026-06-20

- Recorded the stage 02-09 evidence ledger against the audited code commit.
- Added the backend runtime and protocol test target to the CTest matrix.
- Fixed ring overwrite recovery so repeated wraps and forced overwrite remain recoverable.

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
