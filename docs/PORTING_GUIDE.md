# Porting Guide

This guide explains how to add a new backend.

## Backend contract

A backend must provide:

- `read(addr, buf, len)`
- `write(addr, buf, len)`
- for erase-before-write media, an erase primitive with sector semantics

The core assumes:

- address range checks are enforced
- read/write are deterministic
- write either succeeds fully or reports error
- erase either succeeds fully or reports error
- 0 -> 1 programming is forbidden on NOR-like media

## Media classes

### Byte-writable
Examples:
- FRAM
- EEPROM
- RAM simulator

Properties:
- byte writes are allowed
- no erase step
- ring mode is supported

### Erase-before-write
Examples:
- NOR flash
- STM32 internal flash

Properties:
- writes must respect the physical program unit
- no 0 -> 1 transitions without erase
- incomplete tail records must be recoverable
- ring mode is not supported

## Alignment

- The backend should enforce its physical program alignment.
- The core uses `program_unit` to size flash writes.
- Alignment violations are backend errors, not undefined behavior.

## Durability and sync

If the backend exposes file or storage sync, ensure that a successful write means the data is durable enough for the target platform contract.

For the POSIX backend:

- file-backed mode should flush data after writes
- failure injection must model partial writes and power loss

## Timeouts

Backends should fail fast on bus or flash busy timeout rather than hanging forever.

## Required protocol tests

For a new backend, test at least:

- read/write round-trip
- erase and re-read
- 0 -> 1 violation handling if relevant
- partial write failure
- partial erase failure if relevant
- mount after interrupted append
- append after recovery

## New backend checklist

- [ ] implement `read`
- [ ] implement `write`
- [ ] implement `erase` if applicable
- [ ] document program unit
- [ ] document alignment
- [ ] document sync semantics
- [ ] run compile checks
- [ ] add failure-injection tests
- [ ] add mount/recovery tests

