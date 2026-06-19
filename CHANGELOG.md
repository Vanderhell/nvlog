# Changelog

## Unreleased

- Preserved flash geometry metadata through `nvlog_format()` and `nvlog_mount()` so the flash helper's public fields remain live after initialization.
- Added flash-geometry assertions to the host flash backend test.
- Added host coverage for flash program-unit geometry values 1, 4, 8, and 32.
- Removed stale README status claims that overstated verification.
- Normalized public version/status text in headers and test banners.
- Fixed the currently reported compiler warnings in the host build.
- Added compile-only coverage for the FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, and ESP-IDF partition example backends.
- Added public-header compile checks for both C and C++ consumers.
- Updated README evidence language to distinguish host-tested, simulator-tested, and compile-verified items from unverified hardware behavior.
