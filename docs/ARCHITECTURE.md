# Architecture

## Layer model

Application
↓
nvlog public API
↓
media format and recovery core
↓
HAL contract
↓
FRAM / EEPROM / NOR / STM32 / ESP-IDF

## What lives where

### Application
Owns:

- payloads
- mount lifetime
- mutexes or task serialization
- persistence policy

### Public API
Provides:

- `nvlog_format()`
- `nvlog_mount()`
- `nvlog_append()`
- `nvlog_iter_init()`
- `nvlog_iter_next()`
- `nvlog_read_payload()`
- ring equivalents

### Recovery core
Implements:

- record encoding
- CRC checks
- linear recovery
- ring recovery
- stale object detection
- allocation and wrap logic

### HAL contract
Abstracts:

- `read`
- `write`
- optional erase in flash helper layer

### Media
Examples:

- FRAM: byte-writable, no erase
- EEPROM: byte-writable, no erase
- NOR: erase-before-write, 0 -> 1 forbidden
- STM32 internal flash: erase-before-write, geometry-specific program units

## On-media layout

### Superblocks
Two copies are stored on media. They hold:

- magic
- version
- mode
- media class
- region size
- generation
- metadata sequence
- write pointer
- tail pointer
- next sequence
- counters
- geometry key

### Records
Each record contains:

- header
- payload
- CRC
- commit byte

Record types:

- `DATA`
- `WRAP`
- `PADDING`

## Identity and mutation

The core tracks:

- generation
- metadata sequence
- session identity
- mutation counter

These are used to detect stale iterators and stale record snapshots.

## Commit point

The committed record is the record whose CRC and commit byte are both written.

- on byte-writable media, the write becomes visible as soon as it is committed
- on erase-before-write media, incomplete tails are treated as recovery artifacts

## Linear recovery

Mount scans from the start of the region and restores:

- write pointer
- next sequence
- record count

Incomplete tail data is ignored.

## Ring recovery

Mount restores:

- live window
- tail pointer
- write pointer
- count
- next sequence

Ring mode uses `DATA`, `WRAP`, and `PADDING` records to keep the window consistent.

## Atomicity

The goal is old-or-new state after failures, not arbitrary partial state.

What is guaranteed:

- old committed record survives, or
- new committed record survives, or
- incomplete tail is ignored

What is not guaranteed:

- zero-cost recovery
- unlimited overwrite capacity
- hardware behavior beyond the stated backend contract

