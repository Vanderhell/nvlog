# API Reference

## `nvlog_ctx_init()`

Zeroes a caller-owned context before first use.

Use it before the first `nvlog_mount()` on a fresh context. `nvlog_format()`
initializes the context itself.

## `nvlog_format()`

Formats byte-writable or linear media.

Returns:

- `NVLOG_OK` on success
- `NVLOG_ERR_PARAM` for invalid arguments or size
- `NVLOG_ERR_IO` if the backend fails

## `nvlog_mount()`

Mounts an existing linear region and restores the write pointer.

Mount is read-only. Successful remounts refresh the in-memory session identity
so old iterators and record snapshots become stale.

Call `nvlog_ctx_init()` before mounting into a fresh caller-owned context.

Returns `NVLOG_ERR_INCOMPLETE` only internally during recovery scanning; the
public API returns the mapped status.

## `nvlog_append()`

Appends one payload.

Rules:

- payload may be zero-length
- payload must not be `NULL` when length is non-zero
- linear mode can return `NVLOG_ERR_FULL`
- stale context or not-mounted context is rejected

## `nvlog_iter_init()` / `nvlog_iter_next()`

Iterates committed records in sequence order.

The iterator becomes stale after:

- remount
- format
- append on the same context

Corrupt or truncated data is reported through explicit status codes instead of
being silently accepted.

## `nvlog_read_payload()`

Reads payload for a record returned by the iterator.

Requires:

- matching session id
- matching generation
- matching record metadata

The descriptor is a snapshot for the current mounted context and should not be
treated as a durable handle across independent remounts.

## `nvlog_stats()`

Returns:

- used bytes
- free bytes
- record count
- next sequence

## Ring API

### `nvlog_ring_format()`
Formats ring mode for byte-writable media.

### `nvlog_ring_mount()`
Restores ring state after reset. Mount is read-only and refreshes the
in-memory session identity only.

### `nvlog_ring_count()`
Returns the count of valid records in ring mode.

## Lifetime rules

- A context is owned by the caller.
- A mounted context stays valid until the next format, remount, or append that changes its mutation state.
- Iterators and record descriptors are snapshots, not persistent handles.
