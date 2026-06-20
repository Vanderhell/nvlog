# Contributing

## Code style

- C99 only for the core and host C tests
- keep the core warning-clean
- prefer explicit types and controlled arithmetic
- keep comments short and factual

## Core rules

- no heap in the core
- no mutable global state in the core
- no native struct persistence
- use explicit little-endian encoding
- use checked arithmetic for offsets and sizes

## Adding a backend

- implement the HAL contract
- document the media class
- document program unit and alignment
- add failure-injection coverage if relevant
- add mount/recovery coverage

## Adding a test

- prove one behavior per test
- keep the test deterministic
- use the simulator or POSIX backend where possible
- do not weaken assertions to make a test pass

## Required builds

- default host build
- strict warning-as-error build
- any platform-specific compile check relevant to the change

## Commit convention

- use short imperative subjects
- one logical change per commit
- update docs and changelog with the code change

## Pull request checklist

- [ ] build passes
- [ ] tests pass
- [ ] docs updated
- [ ] changelog updated if user-visible
- [ ] no test weakening
- [ ] no AI tool attribution in `Author` or `Co-Authored-By`

