# STAGE 03 - STALE IDENTITY, ITERATOR SAFETY, AND API CONTRACT

## Exact Commit SHA

`ff68b4416d6cec8ca01b252b3522b1e48df24026`

## Changed Files

- `src/nvlog.c`
- `tests/test_powerloss.c`
- `tests/test_ring.c`

## Implementation Facts

- Stale iterators and stale record descriptors are rejected after remount or mutation.
- Identity checks distinguish session, generation, and mutation state instead of treating any post-mount access as valid.
- The power-loss suite exercises interrupted writes and confirms the committed tail remains recoverable.

## Configure / Build / Test Commands

- `cmake --build build-release --config Release -j 4`
- `cmd /c build-release\Release\test_powerloss.exe > C:\tmp\test_powerloss.out 2>&1`
- `ctest --test-dir build-release -C Release --output-on-failure`

## Exact Results

- `test_powerloss.exe`: `PASSED: 25`, `FAILED: 0`
- CTest suites `suite_v02` and `suite_v03`: `PASSED`

## Counts

- Suite count: `1`
- Logical scenario count: `16`
- Assertion / check count: `25`
- Randomized operation count: `0`
- Failure-injection count: `23`

## Explicit Unverified Items

- Physical STM32 execution.
- Physical ESP32 execution.

## Tracked Worktree Status

- Clean immediately after the code commit: `yes`
