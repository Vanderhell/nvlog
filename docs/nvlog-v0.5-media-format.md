# nvlog v0.5 media format

This document defines the on-media encoding for the next nvlog minor format.
It is normative for the implementation work in this repository and is written
in terms of explicit byte fields rather than native C structs.

## Byte order

All multibyte integer fields are encoded in little-endian order.

## Region header

The region header occupies the first 28 bytes of the region.

| Offset | Size | Field | Description |
|---|---:|---|---|
| 0x00 | 4 | magic | ASCII `NVLG` |
| 0x04 | 2 | format_version | `0x0005` for this format |
| 0x06 | 1 | mode | `0x00` linear, `0x01` ring |
| 0x07 | 1 | reserved0 | Must be zero |
| 0x08 | 4 | region_size | Configured region size in bytes |
| 0x0C | 4 | generation | Monotonic session identifier |
| 0x10 | 4 | metadata_seq | Redundant metadata commit sequence |
| 0x14 | 4 | feature_flags | Must be zero for v0.5 |
| 0x18 | 4 | header_crc32 | CRC32 of bytes `0x00..0x17` |

Validation rules:

- `magic` must match.
- `format_version` must equal `0x0005`.
- `mode` must be a known enum value.
- `reserved0` and `feature_flags` must be zero.
- `region_size` must equal the mounted capacity.
- `header_crc32` must validate before any record scan.

## Record header

The record header occupies 20 bytes.

| Offset | Size | Field | Description |
|---|---:|---|---|
| 0x00 | 1 | magic | Record anchor byte |
| 0x01 | 1 | type | `0x01` data, `0x02` wrap/padding |
| 0x02 | 1 | format_version | `0x05` for this format |
| 0x03 | 1 | flags | Must be zero unless explicitly defined |
| 0x04 | 4 | generation | Must match the mounted region header |
| 0x08 | 4 | sequence | Monotonic per generation |
| 0x0C | 2 | payload_len | Payload bytes that follow |
| 0x0E | 2 | total_len | Header + payload + commit bytes |
| 0x10 | 4 | header_crc32 | CRC32 of bytes `0x00..0x0F` |

Commit marker:

- The final 4 bytes of a committed record are the record CRC32.
- The CRC covers the 20-byte record header followed by the payload bytes.
- A record is committed only after the CRC bytes are durable.

## Alignment

- Linear byte-writable media may use byte-granular writes.
- Flash media must align physical writes to the configured program unit.
- Ring recovery must treat wrap markers / padding records as explicit records.

## Session and generation handling

- `generation` is selected by `format`.
- Records from prior generations must not be considered valid.
- A successful format publishes a new region header generation only after the
  new header CRC is committed.

## Wrap behavior

- Sequence comparison is wrap-aware across `uint32_t` wraparound.
- Ring recovery must preserve oldest-to-newest iteration order.
- End-of-region gaps must be represented explicitly rather than inferred.

