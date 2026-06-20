# STAGE 07 - CI, CONSISTENCY GUARDS, AND REPRODUCIBLE EVIDENCE

## Exact Commit SHA

`79a2d1a65d173ebca97e06f9f40843ec3f203ad7`

## Changed Files

- `tools/truth_guard.ps1`
- `.github/workflows/ci.yml`
- `docs/audit/evidence/final_manifest.json`
- `docs/audit/STAGE_COMPLETION_MATRIX.md`

## Implementation Facts

- The truth guard checks the release/docs claims against the actual repository state.
- The CI workflow now includes the Release/Debug, strict warning, sanitizer, 32-bit, consumer, backend protocol, deterministic model, and failure-injection jobs required by the prompt pack.
- The audit ledger is stored in-repo together with a generated manifest so the release evidence is machine-checkable.

## Configure / Build / Test Commands

- `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\truth_guard.ps1`
- `git diff --check`
- `ctest --test-dir build -C Debug --output-on-failure`
- `ctest --test-dir build -C Release --output-on-failure`

## Exact Results

- `truth_guard.ps1`: `PASS`
- `git diff --check`: no diff errors
- `ctest --test-dir build -C Debug --output-on-failure`: `10/10` suites passed
- `ctest --test-dir build -C Release --output-on-failure`: `10/10` suites passed

## Counts

- Suite count: `10`
- Logical scenario count: `30000`
- Assertion / check count: `382517`
- Randomized operation count: `30000`
- Failure-injection count: `49`

## Explicit Unverified Items

- Physical STM32 execution.
- Physical ESP32 execution.

## Tracked Worktree Status

- Clean immediately after the code commit: `yes`
