# nvlog

`nvlog` is a small persistent record buffer for non-volatile storage.

Current repository baseline:

- C99 core library
- explicit v0.5 media encoding with byte-writable and erase-before-write media contracts
- linear and ring APIs in the public header
- POSIX RAM/file backend for host tests
- NOR flash simulator for host tests
- compile-only example backend sources under `backends/hal_examples/`
- public-header compile checks for C and C++

Current release: `v1.0.5`.

The tree verifies the host test suites on Windows with CTest, including MSVC Release/Debug, strict warning-as-error variants, Win32 variants, and ClangCL strict builds. It does not claim physical hardware verification.

## Verified Baseline

- 9 CTest suites in the current tree
- host Release and Debug builds with MSVC on Windows
- strict warning-as-error Release and Debug builds with MSVC
- Win32 Release and strict Release builds with MSVC
- ClangCL strict builds
- host POSIX and flash-simulator test execution
- compile-only example backend builds for FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, and an ESP-IDF partition adapter
- public C and C++ header consumer builds
- direct ring regression and randomized model execution

Current verification levels:

- host-tested: POSIX model, flash simulator, core test suites, ring regression, randomized model
- simulator-tested: NOR flash simulator
- protocol-mock-tested: backend protocol suite
- compile-verified: FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, ESP-IDF partition adapter
- hardware-verified: not verified

Current limitations:

- physical STM32 and ESP32 execution are not verified
- flash-backed runtime behavior is compile-verified only for the example integrations
- claims should be read against the tests and build evidence in this tree

User-facing documentation:

- [Getting Started](docs/GETTING_STARTED.md)
- [Cookbook](docs/COOKBOOK.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Power Loss Contract](docs/POWER_LOSS_CONTRACT.md)
- [Porting Guide](docs/PORTING_GUIDE.md)
- [API Reference](docs/API_REFERENCE.md)
- [Testing](docs/TESTING.md)
- [Capacity Planning](docs/CAPACITY_PLANNING.md)
- [RTOS Integration](docs/RTOS_INTEGRATION.md)
- [Hardware Validation](docs/HARDWARE_VALIDATION.md)
- [Contributing](CONTRIBUTING.md)
- [Security](SECURITY.md)
- [Support](SUPPORT.md)
- [Examples](examples/README.md)

## Build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release
ctest --test-dir build-release -C Release --output-on-failure
```

Debug uses the same steps with `build-debug`.

## Scope

Public claims about power-loss guarantees, flash support, hardware support, footprint, and release status should be read against the tests and build evidence in this tree.

## Evidence Matrix

- Core host tests: host-tested
- POSIX backend: host-tested
- Flash simulator: simulator-tested
- Flash format helper: host-tested
- Flash program-unit geometry: host-tested
- FRAM example backend: compile-verified
- EEPROM example backend: compile-verified
- SPI NOR example backend: compile-verified
- STM32F4 example backend: compile-verified
- STM32L4 example backend: compile-verified
- STM32H7 example backend: compile-verified
- ESP-IDF partition adapter: compile-verified
- STM32 physical execution: not verified
- ESP32 physical execution: not verified
