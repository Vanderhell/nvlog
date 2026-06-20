# Capacity Planning

## Linear mode

Capacity consumed by one record:

`record_alloc = align_up(NVLOG_RECORD_OVERHEAD + payload_len, program_unit)`

Total required media:

`media = NVLOG_REGION_HEADER_SIZE + sum(record_alloc)`

## Ring mode

Ring mode needs:

- media header
- physical reserve
- live records

The usable window is the region size minus the reserve.

## Choosing payload sizes

Smaller payloads cost proportionally more overhead.

For tiny event logs, ring mode may be more efficient than linear mode if you only need the last N events.

## Program unit

Pick the real physical program unit from the target media.

Do not use the logical payload size to infer it.

