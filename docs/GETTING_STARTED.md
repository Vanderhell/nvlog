# Getting Started

This guide shows the shortest correct way to integrate `nvlog` on a byte-writable backend.

## 1. Initialize the context

Always zero or initialize the context before first use.

```c
#include <stdio.h>
#include <string.h>
#include "nvlog.h"
#include "nvlog_posix.h"

int main(void)
{
    nvlog_ctx_t ctx;
    nvlog_ctx_init(&ctx);
    /* ... */
    return 0;
}
```

## 2. Pick a backend

For a first host-side run, use the POSIX RAM backend.

```c
nvlog_posix_ctx_t pctx;
nvlog_hal_t hal;
if (nvlog_posix_open_ram(&pctx, &hal, 4096u) != 0) {
    return 1;
}
```

For a persistent file-backed demo, use `nvlog_posix_open_file()`.

## 3. Format on first boot, mount on later boots

- First boot on blank media: call `nvlog_format()` or `nvlog_ring_format()`.
- Later boot on existing media: call `nvlog_mount()` or `nvlog_ring_mount()`.

```c
if (first_boot) {
    if (nvlog_format(&ctx, &hal, 4096u) != NVLOG_OK) return 1;
} else {
    if (nvlog_mount(&ctx, &hal, 4096u) != NVLOG_OK) return 1;
}
```

Use `nvlog_ring_format()` and `nvlog_ring_mount()` for ring mode.

## 4. Append records

```c
const char payload[] = "hello";
if (nvlog_append(&ctx, payload, sizeof(payload) - 1u) != NVLOG_OK) {
    return 1;
}
```

## 5. Iterate and read payloads

```c
nvlog_iter_t it;
nvlog_record_t rec;
uint8_t buf[64];

if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return 1;
while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
    if (rec.len > sizeof(buf)) return 1;
    if (nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) != NVLOG_OK) return 1;
    printf("seq=%u len=%u\n", (unsigned)rec.seq, (unsigned)rec.len);
}
```

## 6. Handle errors

Typical return values:

- `NVLOG_OK`: success
- `NVLOG_ERR_NO_DATA`: iterator exhausted
- `NVLOG_ERR_FULL`: linear log is full
- `NVLOG_ERR_IO`: backend I/O failed
- `NVLOG_ERR_STALE`: iterator or record snapshot is no longer valid

## 7. Full minimal example

```c
#include <stdio.h>
#include <string.h>
#include "nvlog.h"
#include "nvlog_posix.h"

int main(void)
{
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    nvlog_ctx_t ctx;
    nvlog_iter_t it;
    nvlog_record_t rec;
    uint8_t payload[64];

    nvlog_ctx_init(&ctx);
    if (nvlog_posix_open_ram(&pctx, &hal, 4096u) != 0) return 1;

    if (nvlog_format(&ctx, &hal, 4096u) != NVLOG_OK) return 1;
    if (nvlog_append(&ctx, "boot-1", 6u) != NVLOG_OK) return 1;
    if (nvlog_append(&ctx, "boot-2", 6u) != NVLOG_OK) return 1;

    if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return 1;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
        if (rec.len > sizeof(payload)) return 1;
        if (nvlog_read_payload(&ctx, &rec, payload, sizeof(payload)) != NVLOG_OK) return 1;
        fwrite(payload, 1, rec.len, stdout);
        fputc('\n', stdout);
    }

    nvlog_posix_close(&pctx);
    return 0;
}
```

## 8. Linear vs ring

- Linear mode is simplest. Records only grow until the media is full.
- Ring mode is for fixed-size event history. New records overwrite the oldest ones.
- Do not use ring mode on erase-before-write media.

## 9. Recovery model

- Linear mode after power loss: remount and keep appending.
- Ring mode after reset: remount and continue from the latest committed state.
- For erase-before-write backends, incomplete records are treated as tail dirt and skipped.

