# Power Loss Contract

This document states what `nvlog` guarantees during interruption.

## Guaranteed

- committed records remain readable after remount
- incomplete append attempts do not become committed records
- stale iterators and stale record descriptors are rejected
- the core does not rely on native struct persistence

## Not guaranteed

- atomicity across arbitrary backend bugs
- recovery from media that violates its own write or erase contract
- concurrent access without external serialization

## Old-or-new state

For a single append interrupted by power loss:

- the old committed log state may remain
- the new committed log state may appear
- any incomplete tail must be ignored

No mixed committed state is allowed.

## Incomplete record

An incomplete record is a record whose header is visible but whose payload, CRC, or commit marker is not fully committed.

Recovery must treat it as tail dirt.

## Corrupt committed record

If a record was committed but later becomes corrupted, the core reports corruption rather than guessing.

## Backend requirements

### Byte-writable media

- writes may be smaller than a record
- failure injection must model partial write loss
- mount must be able to continue after a tail interruption

### Erase-before-write media

- all writes must respect program unit boundaries
- partial writes must never create false committed data
- incomplete tails are skipped and the write pointer advances past the dirty allocation

## Backend differences

- FRAM: byte-writable, easiest recovery model
- EEPROM: byte-writable, same class as FRAM for nvlog
- NOR: erase-before-write, 0 -> 1 forbidden
- internal flash: erase-before-write, family-specific geometry

## POSIX sync contract

For the POSIX file backend:

- writes should be flushed consistently with the selected test mode
- simulated failures must model the same states as the physical contract

## Failure-injection tests

Each failure-injection test exists to prove one specific recovery property:

- interrupted write: old-or-new state
- interrupted erase: mount rejects corruption
- repeated append after recovery: progress continues
- wrap/overwrite failure: ring remains valid

