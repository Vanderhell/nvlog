# nvlog

`nvlog` is a small persistent record buffer for non-volatile storage.

Current repository baseline:

- C99 core library
- linear and ring APIs in the public header
- POSIX RAM/file backend for host tests
- NOR flash simulator for host tests
- flash-format helper that preserves geometry metadata in `nvlog_ctx_t`
- compile-only example backend sources under `backends/hal_examples/`
- public-header compile checks for C and C++

The tree currently verifies the host test suites on Windows with CTest. It does not claim physical hardware verification.

## Verified baseline

- 8 CTest suites in the current tree
- host Release and Debug builds with MSVC on Windows
- strict warning-as-error Release and Debug builds with MSVC
- host POSIX and flash-simulator test execution
- compile-only example backend builds for FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, and an ESP-IDF partition adapter
- public C and C++ header consumer builds

Current verification levels:

- host-tested: POSIX model, flash simulator, core test suites
- compile-verified: FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, ESP-IDF partition adapter
- hardware-verified: not verified

Current limitations:

- physical STM32 and ESP32 execution are not verified
- flash-backed runtime behavior is compile-verified only for the example integrations
- the repository is still in active repair; claims should be read against the tests and build evidence in this tree

## Build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release
ctest --test-dir build-release -C Release --output-on-failure
```

Debug uses the same steps with `build-debug`.

## Scope

The repository is in active repair. Public claims about power-loss guarantees, flash support, hardware support, footprint, and release status should be treated as pending until they are re-verified in code and tests.

## Evidence Matrix

- Core host tests: host-tested
- POSIX backend: host-tested
- Flash simulator: simulator-tested
- Flash format helper: host-tested
- FRAM example backend: compile-verified
- EEPROM example backend: compile-verified
- SPI NOR example backend: compile-verified
- STM32F4 example backend: compile-verified
- STM32L4 example backend: compile-verified
- STM32H7 example backend: compile-verified
- ESP-IDF partition adapter: compile-verified
- STM32 physical execution: not verified
- ESP32 physical execution: not verified
