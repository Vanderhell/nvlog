# STAGE 01 - PREFLIGHT, RELEASE TRUTH, AND BASELINE LOCK

## Exact Commit SHA

`7af2b17b2c4b2c2b4f76d2a3f8e3a8d24efb3fa5`

## Changed Files

- `CHANGELOG.md`
- `CMakeLists.txt`
- `docs/audit/STAGE_COMPLETION_MATRIX.md`
- `docs/audit/evidence/STAGE_01.md`
- `tools/truth_guard.ps1`

## Implementation Facts

- Added the stage completion ledger required by the audit prompt.
- Added a PowerShell truth guard that validates README release wording, changelog/version consistency, tag-to-changelog consistency, and stage ledger completeness.
- Added a CTest entry so the same guard runs under the repository test harness.
- Added a dated `1.0.1` changelog release section so the existing annotated `v1.0.1` tag has a matching release entry.

## Configure / Build / Test Commands

- `git status --short`
- `git branch --show-current`
- `git rev-parse HEAD`
- `git log -15 --oneline`
- `git tag --list --sort=version:refname`
- `git remote -v`
- `git show-ref --tags`
- `git ls-remote --tags origin`
- `git cat-file -t v1.0.1`
- `git rev-list -n 1 v1.0.1`
- `git for-each-ref refs/tags/v1.0.1 --format='%(refname) %(objecttype) %(objectname) %(taggername) %(taggerdate:iso8601) %(subject)'`
- `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\truth_guard.ps1`

## Exact Results

- Local branch: `master`
- HEAD: `7af2b17b2c4b2c2b4f76d2a3f8e3a8d24efb3fa5`
- Local tags: `v1.0.0`, `v1.0.1`
- `v1.0.1` is an annotated tag.
- `v1.0.1` resolves to `6b285f087d0607e7ad184602e332d20ff5e5ad0f`.
- Remote tag enumeration failed because the environment cannot reach `github.com`.
- Truth guard passes locally after the stage-1 edits.

## Counts

- Suite count: `1`
- Logical scenario count: `4`
- Assertion / check count: `12`
- Randomized operation count: `0`
- Failure-injection count: `0`

## Explicit Unverified Items

- Remote tag enumeration could not be completed because network access to `github.com` is blocked in this environment.
- Physical STM32 execution.
- Physical ESP32 execution.

## Tracked Worktree Status

- Clean immediately after the stage-1 commit: `yes`
