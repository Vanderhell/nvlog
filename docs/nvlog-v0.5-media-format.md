# nvlog v0.5 media format

This document describes the currently implemented v0.5 media encoding in this repository.

## Scope

- Public API version: `1.0.6`
- Supported media classes:
  - byte-writable media
  - erase-before-write media
- Supported logical modes:
  - linear
  - ring

Physical STM32 and ESP32 execution are not verified.

## Byte order

All multibyte integer fields are encoded little-endian.

## Region layout

The first 128 bytes of a formatted region contain two independent 64-byte superblock copies:

- copy A at offset `0x0000`
- copy B at offset `0x0040`

The media payload begins at offset `0x0080`.

## Superblock encoding

Each superblock is 64 bytes and uses explicit fixed-width fields:

| Offset | Size | Field |
|---|---:|---|
| 0x00 | 4 | `magic` |
| 0x04 | 2 | `format_version` |
| 0x06 | 1 | `mode` |
| 0x07 | 1 | `media_class` |
| 0x08 | 4 | `region_size` |
| 0x0C | 4 | `generation` |
| 0x10 | 4 | `metadata_seq` |
| 0x14 | 4 | `write_ptr` |
| 0x18 | 4 | `tail_ptr` |
| 0x1C | 4 | `next_seq` |
| 0x20 | 4 | `record_count` |
| 0x24 | 4 | `used_bytes` |
| 0x28 | 4 | `free_bytes` |
| 0x2C | 4 | `padding_bytes` |
| 0x30 | 4 | `reserve_bytes` |
| 0x34 | 4 | `feature_flags` |
| 0x38 | 4 | `reserved0` |
| 0x3C | 4 | `crc32` |

Validation rules:

- `magic` must equal `NVLOG_MEDIA_MAGIC`
- `format_version` must equal `NVLOG_MEDIA_VERSION`
- `mode` must be supported
- `media_class` must match the mounted media contract
- `reserved0` stores the program unit for the mounted media
- `feature_flags` stores the geometry key for the mounted media
- `crc32` must match the CRC of bytes `0x00..0x37`
- if both copies are valid, the newer generation and metadata sequence are selected using wrap-aware ordering

## Record encoding

Each record uses a 32-byte header, followed by payload bytes, followed by a 4-byte payload CRC, followed by a 1-byte final commit state.

### Record header

| Offset | Size | Field |
|---|---:|---|
| 0x00 | 1 | `magic` |
| 0x01 | 1 | `type` |
| 0x02 | 1 | `version` |
| 0x03 | 1 | `flags` |
| 0x04 | 1 | `mode` |
| 0x05 | 3 | `reserved` |
| 0x08 | 4 | `generation` |
| 0x0C | 4 | `seq` |
| 0x10 | 4 | `payload_len` |
| 0x14 | 4 | `total_len` |
| 0x18 | 4 | `alloc_len` |
| 0x1C | 4 | `crc32` |

Supported record types:

- `DATA`
- `WRAP`
- `PADDING`

Validation rules:

- `magic` must equal `NVLOG_RECORD_MAGIC`
- `version` must equal `NVLOG_RECORD_VERSION`
- `type` must be one of the supported types
- `flags` must be one of the supported flags
- `mode` must be supported
- `reserved` bytes must be zero
- `generation` must match the mounted generation
- `payload_len`, `total_len`, and `alloc_len` must be internally consistent
- header CRC must match the encoded header bytes

## Final commit state

The record commit state is a separate 1-byte field written after the payload CRC.

- `0x00` means committed
- `0xFF` means not committed / erased

The implementation validates the final commit byte before exposing a record.

## Recovery contract

The implementation distinguishes:

- clean erased end
- incomplete uncommitted record
- corrupt committed record
- unsupported record or format
- mode mismatch
- generation mismatch
- bounds violation
- backend read/write failure
- stale iterator or stale descriptor

Ring mode recovery restores:

- head
- tail
- write position
- live record count
- live payload bytes
- padding bytes
- reserve bytes
- free bytes
- next sequence

## Verification status

Verified in this repository:

- host-tested: POSIX host model, ring tests, randomized model, core tests, power-loss tests
- simulator-tested: flash simulator
- protocol-mock-tested: backend protocol suite
- compile-verified: FRAM, EEPROM, SPI NOR, STM32F4, STM32L4, STM32H7, ESP-IDF partition adapter
- hardware-verified: not verified

## Notes

- This repository does not claim generic hardware verification.
- Physical STM32 and ESP32 execution remain not verified.
- The document should be read together with the test evidence and build evidence in the repository.
