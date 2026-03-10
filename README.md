# nvlog

**Tiny persistent record buffer for MCUs.**  
Power-loss safe binary log for non-volatile memory — no filesystem, no heap, no dependencies.

---

## Modes

| Mode | Description | Backends |
|------|-------------|----------|
| `NVLOG_MODE_LINEAR` | Append-only, stops when full | FRAM, EEPROM, NOR flash, STM32 internal flash |
| `NVLOG_MODE_RING` | Circular, overwrites oldest records | FRAM, EEPROM, RAM (byte-writable only) |

---

## Record format

```
[MAGIC:1][FLAGS:1][LEN:2][SEQ:4][PAYLOAD:N][CRC32:4]
```

CRC32 is written last — it is the commit point. Interrupted writes leave no visible record after `nvlog_mount()`.

---

## Quick start

```c
#include "nvlog.h"

// 1. implement HAL for your media
nvlog_hal_t hal = {
    .read  = my_fram_read,
    .write = my_fram_write,
    .user  = &my_device,
};

nvlog_ctx_t ctx;

// LINEAR — first boot
nvlog_format(&ctx, &hal, REGION_SIZE);

// LINEAR — subsequent boots
nvlog_mount(&ctx, &hal, REGION_SIZE);

// RING — first boot
nvlog_ring_format(&ctx, &hal, REGION_SIZE);

// RING — subsequent boots
nvlog_ring_mount(&ctx, &hal, REGION_SIZE);

// append (both modes)
nvlog_append(&ctx, &event, sizeof(event));

// iterate oldest-first (both modes)
nvlog_iter_t it; nvlog_record_t rec;
nvlog_iter_init(&it, &ctx);
while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
    uint8_t buf[256];
    nvlog_read_payload(&ctx, &rec, buf, sizeof(buf));
}
```

---

## API

```c
// linear
nvlog_status_t nvlog_format     (nvlog_ctx_t*, const nvlog_hal_t*, uint32_t size);
nvlog_status_t nvlog_mount      (nvlog_ctx_t*, const nvlog_hal_t*, uint32_t size);

// ring
nvlog_status_t nvlog_ring_format(nvlog_ctx_t*, const nvlog_hal_t*, uint32_t size);
nvlog_status_t nvlog_ring_mount (nvlog_ctx_t*, const nvlog_hal_t*, uint32_t size);
uint32_t       nvlog_ring_count (nvlog_ctx_t*);

// write (both modes)
nvlog_status_t nvlog_append     (nvlog_ctx_t*, const void *payload, uint16_t len);

// read (both modes)
nvlog_status_t nvlog_iter_init    (nvlog_iter_t*, nvlog_ctx_t*);
nvlog_status_t nvlog_iter_next    (nvlog_iter_t*, nvlog_record_t*);
nvlog_status_t nvlog_read_payload (nvlog_ctx_t*, const nvlog_record_t*, void*, uint16_t);

// info
nvlog_status_t nvlog_stats      (nvlog_ctx_t*, nvlog_stats_t*);

// flash backends
nvlog_status_t nvlog_flash_format        (nvlog_ctx_t*, const nvlog_hal_flash_t*, uint32_t);
nvlog_status_t nvlog_flash_verify_erased (const nvlog_hal_flash_t*, uint32_t);
```

---

## HAL

```c
// byte-writable (FRAM, EEPROM, RAM)
typedef struct {
    int  (*read) (uint32_t addr, void *buf, uint32_t len, void *user);
    int  (*write)(uint32_t addr, const void *buf, uint32_t len, void *user);
    void *user;
} nvlog_hal_t;

// flash (extends nvlog_hal_t)
typedef struct {
    nvlog_hal_t  base;          // MUST be first
    int        (*erase)(uint32_t addr, uint32_t len, void *user);
    uint32_t     erase_size;
    uint32_t     prog_size;
    void        *user;
} nvlog_hal_flash_t;
```

---

## Backends

| File | Media | Transport |
|------|-------|-----------|
| `backends/nvlog_posix.c` | RAM / file | host only (test/sim) |
| `backends/nvlog_flash_sim.c` | NOR sim | host only (enforces physics) |
| `backends/hal_examples/nvlog_hal_fram.c` | FRAM | SPI (MB85RS) + I2C (MB85RC, FM24) |
| `backends/hal_examples/nvlog_hal_eeprom.c` | EEPROM | I2C (AT24Cxx, M24Cxx) |
| `backends/hal_examples/nvlog_hal_nor_spi.c` | NOR flash | SPI (W25Qxx, GD25Q, MX25L) |
| `backends/hal_examples/nvlog_hal_stm32_flash.c` | Internal flash | STM32 F4 / L4 / H7 |

---

## Tests

```bash
cmake -B build && cmake --build build && ctest --test-dir build -V
```

| Suite | File | Tests | Covers |
|-------|------|-------|--------|
| v0.1 | `test_nvlog.c` | 59 | append, iter, CRC, mount, recovery |
| v0.2 | `test_powerloss.c` | 23 | power-loss at every write point |
| v0.3 | `test_flash.c` | 33 | NOR physics, erase, flash format |
| v0.4 | `test_ring.c` | 71 | ring wrap, eviction, ring_mount |
| **total** | | **186** | |

---

## Footprint (GCC -Os, Cortex-M0)

| | Size |
|-|------|
| Code (text) | ~1.2 KB |
| RAM per ctx | 28 B |
| RAM per iter | 20 B |
| Dependencies | none |

---

## Roadmap

| Version | Status | Scope |
|---------|--------|-------|
| v0.1 | ✅ | Linear, CRC32, POSIX backend |
| v0.2 | ✅ | FRAM/EEPROM HAL, power-loss tests |
| v0.3 | ✅ | NOR flash, STM32 internal flash |
| v0.4 | ✅ | Ring mode (circular buffer) |

---

## License

MIT
