# nvlog

`nvlog` is a small persistent record buffer for non-volatile storage.

Current repository baseline:

- C99 core library
- linear and ring APIs in the public header
- POSIX RAM/file backend for host tests
- NOR flash simulator for host tests
- example backend sources under `backends/hal_examples/`

The tree currently verifies the host test suites on Windows with CTest. It does not claim physical hardware verification.

## Verified baseline

- 5 CTest suites in the current tree
- host Release and Debug builds with MSVC on Windows
- host POSIX and flash-simulator test execution

## Build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release
ctest --test-dir build-release -C Release --output-on-failure
```

Debug uses the same steps with `build-debug`.

## Scope

The repository is in active repair. Public claims about power-loss guarantees, flash support, hardware support, footprint, and release status should be treated as pending until they are re-verified in code and tests.
