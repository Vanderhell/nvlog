# STAGE 05 - ERASE-BEFORE-WRITE FLASH RECOVERY

## Exact Commit SHA

`ff68b4416d6cec8ca01b252b3522b1e48df24026`

## Changed Files

- `src/nvlog.c`
- `tests/test_flash.c`
- `backends/nvlog_flash_sim.h`

## Implementation Facts

- Flash recovery keeps committed records visible across power loss and leaves interrupted records invisible.
- Dirty allocations remain occupied until the append or format path completes legally.
- The flash suite exercises supported program-unit sizes and explicit erase-before-write recovery cases.

## Configure / Build / Test Commands

- `cmake --build build-release --config Release -j 4`
- `cmd /c build-release\Release\test_flash.exe > C:\tmp\test_flash.out 2>&1`
- `ctest --test-dir build-release -C Release --output-on-failure`

## Exact Results

- `test_flash.exe`: `PASSED: 74`, `FAILED: 0`
- CTest suite `suite_v05`: `PASSED`

## Counts

- Suite count: `1`
- Logical scenario count: `22`
- Assertion / check count: `74`
- Randomized operation count: `0`
- Failure-injection count: `4`

## Explicit Unverified Items

- Physical STM32 execution.
- Physical ESP32 execution.

## Tracked Worktree Status

- Clean immediately after the code commit: `yes`
