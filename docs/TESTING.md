# Testing

## Test matrix

- core linear tests
- power-loss tests
- flash simulator tests
- ring tests
- wire format tests
- public header compile tests
- backend integration compile tests

## Model

The repository uses a host-side model to exercise:

- append behavior
- mount/recovery behavior
- stale iterator behavior
- wrap behavior
- power-loss behavior

## Failure injection

Used to simulate:

- failed write after N operations
- partial write
- failed erase after N operations
- partial erase

## Sanitizers and strict builds

Strict builds are used to catch:

- warnings
- signedness mistakes
- conversion mistakes
- missing prototypes

## What a passing test means

Passing tests prove the contract in the repository tree, not unbounded physical hardware behavior.

