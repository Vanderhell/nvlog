# STAGE 04 - RING REPRESENTATION AND OLD-OR-NEW ATOMICITY

## Exact Commit SHA

`ff68b4416d6cec8ca01b252b3522b1e48df24026`

## Changed Files

- `src/nvlog.c`
- `tests/test_ring.c`
- `CMakeLists.txt`

## Implementation Facts

- The ring append path now keeps wrap and small-gap handling bounded and recoverable.
- Old data is not retired until after the new record is committed and the final metadata state is validated.
- The stress test covers exact old-or-new overwrite behavior, exact stats recovery, stale descriptor invalidation, and repeated wraps.

## Configure / Build / Test Commands

- `cmake --build build-release --config Release -j 4`
- `cmd /c build-release\Release\test_ring.exe > C:\tmp\ring.out 2>&1`
- `ctest --test-dir build-release -C Release --output-on-failure`

## Exact Results

- `test_ring.exe`: `PASSED: 834`, `FAILED: 0`
- CTest suite `suite_v04`: `PASSED`

## Counts

- Suite count: `1`
- Logical scenario count: `30`
- Assertion / check count: `834`
- Randomized operation count: `0`
- Failure-injection count: `5`

## Explicit Unverified Items

- Physical STM32 execution.
- Physical ESP32 execution.

## Tracked Worktree Status

- Clean immediately after the code commit: `yes`
