# nvlog

[![CI](https://github.com/Vanderhell/nvlog/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Vanderhell/nvlog/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Vanderhell/nvlog)](https://github.com/Vanderhell/nvlog/releases)
[![License](https://img.shields.io/github/license/Vanderhell/nvlog)](LICENSE)
[![C99](https://img.shields.io/badge/C99-core-blue)](include/nvlog.h)

`nvlog` is a small persistent record buffer for non-volatile storage.

It is designed for embedded and host-simulated media where record recovery, deterministic encoding, and power-loss behavior matter more than a generic logging API.

Current release: `v1.0.6`.

## Release Snapshot

- C99 core library
- explicit v0.5 media encoding with byte-writable and erase-before-write media contracts
- linear and ring APIs in the public header
- POSIX RAM/file backend for host tests
- NOR flash simulator for host tests
- compile-verified example backend sources under `backends/hal_examples/`
- public-header compile checks for C and C++
- release documentation, cookbook, porting guide, and power-loss contract

## What To Read First

- [Getting Started](docs/GETTING_STARTED.md)
- [Cookbook](docs/COOKBOOK.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Power Loss Contract](docs/POWER_LOSS_CONTRACT.md)
- [Porting Guide](docs/PORTING_GUIDE.md)
- [API Reference](docs/API_REFERENCE.md)
- [Testing](docs/TESTING.md)
- [Examples](examples/README.md)

If you only want to integrate the library, start with [Getting Started](docs/GETTING_STARTED.md) and then pick a recipe from [Cookbook](docs/COOKBOOK.md).

## Quick Start

The public flow is:

1. bind a HAL
2. call `nvlog_format()` on the first boot
3. call `nvlog_mount()` on later boots
4. append records
5. iterate and read payloads

```c
#include "nvlog.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_storage[4096u];

static int ram_read(uint32_t addr, void *buf, uint32_t len, void *user) {
    uint8_t *mem = (uint8_t *)user;
    memcpy(buf, mem + addr, len);
    return 0;
}

static int ram_write(uint32_t addr, const void *buf, uint32_t len, void *user) {
    uint8_t *mem = (uint8_t *)user;
    memcpy(mem + addr, buf, len);
    return 0;
}

int main(void) {
    nvlog_hal_t hal = {
        .read = ram_read,
        .write = ram_write,
        .user = g_storage,
    };
    nvlog_ctx_t log;
    nvlog_iter_t it;
    nvlog_record_t rec;
    uint8_t payload[] = {0x11u, 0x22u, 0x33u};
    uint8_t out[sizeof(payload)];

    memset(g_storage, 0xFF, sizeof(g_storage));
    nvlog_ctx_init(&log);

    if (nvlog_format(&log, &hal, (uint32_t)sizeof(g_storage)) != NVLOG_OK) {
        return 1;
    }
    if (nvlog_append(&log, payload, sizeof(payload)) != NVLOG_OK) {
        return 1;
    }
    if (nvlog_iter_init(&it, &log) != NVLOG_OK) {
        return 1;
    }
    if (nvlog_iter_next(&it, &rec) != NVLOG_OK) {
        return 1;
    }
    if (nvlog_read_payload(&log, &rec, out, sizeof(out)) != NVLOG_OK) {
        return 1;
    }

    return (memcmp(payload, out, sizeof(payload)) == 0) ? 0 : 1;
}
```

For a complete buildable example, see:

- [minimal linear example](examples/minimal_linear/README.md)
- [minimal ring example](examples/minimal_ring/README.md)
- [POSIX file example](examples/posix_file/README.md)

## Build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release
ctest --test-dir build-release -C Release --output-on-failure
```

Debug uses the same steps with `build-debug`.

## Verification Surface

- 9 CTest suites in the current tree
- host Release and Debug builds with MSVC on Windows
- strict warning-as-error Release and Debug builds with MSVC
- Win32 Release and strict Release builds with MSVC
- ClangCL strict builds
- host POSIX and flash-simulator test execution
- compile-only example backend builds for FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, and an ESP-IDF partition adapter
- public C and C++ header consumer builds
- direct ring regression and randomized model execution

## Supported Surface

- host-tested: POSIX model, flash simulator, core test suites, ring regression, randomized model
- simulator-tested: NOR flash simulator
- protocol-mock-tested: backend protocol suite
- compile-verified: FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, ESP-IDF partition adapter
- hardware-verified: not verified

## Limitations

- physical STM32 and ESP32 execution are not verified
- flash-backed runtime behavior is compile-verified only for the example integrations
- claims should be read against the tests and build evidence in this tree

## Documentation Index

- [Contributing](CONTRIBUTING.md)
- [Security](SECURITY.md)
- [Support](SUPPORT.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Hardware Validation](docs/HARDWARE_VALIDATION.md)
- [RTOS Integration](docs/RTOS_INTEGRATION.md)
- [Capacity Planning](docs/CAPACITY_PLANNING.md)

## Examples

- [examples/README.md](examples/README.md) for the example matrix
- [examples/minimal_linear/README.md](examples/minimal_linear/README.md)
- [examples/minimal_ring/README.md](examples/minimal_ring/README.md)
- [examples/posix_file/README.md](examples/posix_file/README.md)
