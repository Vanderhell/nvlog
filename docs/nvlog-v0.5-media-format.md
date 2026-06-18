# nvlog media-format baseline

This document describes the on-media layout currently implemented in this
repository. It is intentionally explicit about the parts that are verified
here and avoids claiming a completed flash-random-access redesign that is not
yet present.

## Byte order

All multibyte integer fields are encoded in little-endian order.

## Region metadata

The first 64 bytes of the region are a redundant A/B superblock pair.

| Offset | Size | Field | Description |
|---|---:|---|---|
| 0x00 | 4 | magic | `NVLG` |
| 0x04 | 2 | format_version | `0x0005` |
| 0x06 | 1 | mode | `0x00` linear, `0x01` ring |
| 0x07 | 1 | reserved0 | Must be zero |
| 0x08 | 4 | region_size | Configured region size in bytes |
| 0x0C | 4 | generation | Session identifier selected by format |
| 0x10 | 4 | metadata_seq | Publication counter for metadata writes |
| 0x14 | 4 | feature_flags | Must be zero |
| 0x18 | 4 | crc32 | CRC32 of bytes `0x00..0x17` |
| 0x20 | 32 | copy B | Second copy with the same structure |

Validation rules:

- Both copies are validated independently.
- If both copies are valid, the newer generation is selected.
- Reserved fields must be zero.
- Mode and region size must match the mounted API call.

## Record layout

The current implementation still uses the existing byte-writable record
encoding:

- 8-byte logical record header
- variable payload
- 4-byte record CRC written last
- 24-byte reserved allocation unit per appended record

The logical header fields are:

| Offset | Size | Field | Description |
|---|---:|---|---|
| 0x00 | 1 | magic | Record anchor byte |
| 0x01 | 1 | flags | Mode flag |
| 0x02 | 2 | len | Payload length |
| 0x04 | 4 | seq | Sequence number |

The CRC is computed over the logical header and payload and is written last.

## Implementation notes

- `nvlog_format()` and `nvlog_ring_format()` publish a new metadata
  generation through the redundant superblock pair.
- `nvlog_iter_next()` rejects stale iterators.
- `nvlog_read_payload()` revalidates the record descriptor before copying.
- Flash example backends are compile-verified, not hardware-verified.

## Limitations

- Physical STM32 and ESP32 execution are not verified.
- The current tree does not yet implement the full A/B recovery and flash-ring
  redesign described in `exec.docx`.
- The current tree should be treated as a work-in-progress implementation with
  honest verification boundaries.
