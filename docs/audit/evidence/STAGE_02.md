# STAGE 02 - CORE RECOVERY AND STATUS SEPARATION

## Exact Commit SHA

`ff68b4416d6cec8ca01b252b3522b1e48df24026`

## Changed Files

- `src/nvlog.c`
- `tests/test_nvlog.c`
- `CMakeLists.txt`

## Implementation Facts

- Mounting now preserves explicit recovery status for incomplete, corrupt, version-mismatched, type-mismatched, reserved-field, and media-mismatched records.
- Linear recovery stops at the first clean erased end or incomplete tail instead of swallowing arbitrary verification errors.
- The regression suite covers valid committed records, erased ends, incomplete tails, corrupt committed data, and unsupported formats explicitly.

## Configure / Build / Test Commands

- `cmake --build build-release --config Release -j 4`
- `cmd /c build-release\Release\test_nvlog.exe > C:\tmp\test_nvlog.out 2>&1`
- `ctest --test-dir build-release -C Release --output-on-failure`

## Exact Results

- `test_nvlog.exe`: `PASSED: 99`, `FAILED: 0`
- CTest suite `suite_v01`: `PASSED`

## Counts

- Suite count: `1`
- Logical scenario count: `24`
- Assertion / check count: `99`
- Randomized operation count: `0`
- Failure-injection count: `0`

## Explicit Unverified Items

- Physical STM32 execution.
- Physical ESP32 execution.

## Tracked Worktree Status

- Clean immediately after the code commit: `yes`
