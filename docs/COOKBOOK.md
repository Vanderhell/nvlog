# Cookbook

Practical integration recipes for common nvlog use cases.

## 1. Persistent log on FRAM

### Problem
You want a durable append-only log on FRAM.

### Assumptions
- Byte-writable backend
- No erase operation required
- You keep the HAL alive for the lifetime of the context

### Code
```c
nvlog_ctx_t ctx;
nvlog_ctx_init(&ctx);
if (nvlog_format(&ctx, &fram_hal, fram_size) != NVLOG_OK) return;

if (nvlog_append(&ctx, "event", 5u) != NVLOG_OK) return;
```

### Result
Records append directly and remount by scanning existing state.

### Limits
- Use linear mode if you want an ever-growing history.
- Use ring mode only if your FRAM capacity is fixed and overwrite is acceptable.

## 2. EEPROM configuration journal

### Problem
Store small configuration revisions safely.

### Code
```c
if (nvlog_mount(&ctx, &eeprom_hal, eeprom_size) == NVLOG_OK) {
    /* existing log mounted successfully */
} else if (eeprom_is_blank()) {
    if (nvlog_format(&ctx, &eeprom_hal, eeprom_size) != NVLOG_OK) return;
} else {
    /* surface the mount error instead of formatting blindly */
    return;
}
```

Write fixed-size config records and decode them by sequence number.

## 3. Ring recorder for last N events

### Problem
Keep the last N events only.

### Code
```c
nvlog_ctx_init(&ctx);
if (nvlog_ring_format(&ctx, &hal, ring_size) != NVLOG_OK) return;
```

Append with `nvlog_append()`. Use `nvlog_ring_mount()` on reboot.

### Expected result
The newest records remain available; the oldest ones are overwritten.

## 4. Append after reboot

### Problem
You need to continue writing after a reset.

### Pattern
```c
if (nvlog_mount(&ctx, &hal, size) != NVLOG_OK) {
    if (nvlog_format(&ctx, &hal, size) != NVLOG_OK) return;
}
```

If mount succeeds, you can append immediately.

## 5. Full linear log

### Problem
The log fills up.

### Pattern
```c
nvlog_status_t st = nvlog_append(&ctx, payload, len);
if (st == NVLOG_ERR_FULL) {
    /* archive, compact, or format again */
}
```

### Limit
Linear mode does not recycle space.

## 6. Safe iterator use

### Problem
You want to read all records safely.

### Pattern
```c
nvlog_iter_t it;
nvlog_record_t rec;
if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return;
while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
    /* read payload */
}
```

### Rule
Do not reuse an iterator after remount, format, or append on the same context.

## 7. Stale iterator and stale descriptor

### Problem
An iterator or record snapshot survives a remount.

### Behavior
- `nvlog_iter_next()` returns `NVLOG_ERR_STALE`
- `nvlog_read_payload()` returns `NVLOG_ERR_STALE`

### Reason
The context session id changes on successful mount/format.

## 8. Capacity planning

### Problem
You need to know how much media you need.

### Formula
- Linear: `region_size >= media header + sum(record_alloc)`
- Ring: `region_size >= media header + reserve + live data`

Use `NVLOG_RECORD_OVERHEAD` plus payload length, then align by the backend program unit.

## 9. Program unit selection

### Rule
Choose the physical program unit from the backend:
- `1` for byte-writable media
- `4` for STM32F4-style word programming
- `8` for STM32L4-style double-word programming
- `32` for STM32H7-style flash word programming

Never hardcode the unit in application logic.

## 10. Recovery after interrupted write

### Problem
Power loss or reset happens during append.

### Behavior
- Old committed record remains valid, or
- New committed record becomes valid, or
- Incomplete tail is skipped on remount

### Example
Use the flash simulator or POSIX fail injection to verify this path.

## 11. Export all records

### Pattern
```c
nvlog_iter_t it;
nvlog_record_t rec;
uint8_t buf[256];

if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return;
while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
    if (rec.len > sizeof(buf)) break;
    if (nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) != NVLOG_OK) break;
}
```

## 12. Reset by formatting

### Problem
You want to clear the log.

### Pattern
Call `nvlog_format()` or `nvlog_ring_format()` again.

## 13. FreeRTOS and Zephyr

### Rule
Protect one `nvlog_ctx_t` with one mutex.

### Do not
- call the API from ISR context
- share a context without serialization
