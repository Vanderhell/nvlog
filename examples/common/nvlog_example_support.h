#ifndef NVLOG_EXAMPLE_SUPPORT_H
#define NVLOG_EXAMPLE_SUPPORT_H

#include <string.h>
#include "../../include/nvlog.h"

typedef struct {
    uint8_t  *storage;
    uint32_t  size;
} nvlog_example_ram_backend_t;

static int nvlog_example_ram_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_example_ram_backend_t *b = (nvlog_example_ram_backend_t *)user;
    if (!b || !buf) return -1;
    if (addr > b->size || len > b->size - addr) return -1;
    memcpy(buf, b->storage + addr, len);
    return 0;
}

static int nvlog_example_ram_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_example_ram_backend_t *b = (nvlog_example_ram_backend_t *)user;
    const uint8_t *src = (const uint8_t *)buf;
    if (!b || (!src && len > 0)) return -1;
    if (addr > b->size || len > b->size - addr) return -1;
    if (len > 0) memcpy(b->storage + addr, src, len);
    return 0;
}

static void nvlog_example_ram_bind(nvlog_hal_t *hal, nvlog_example_ram_backend_t *backend, uint8_t *storage, uint32_t size)
{
    backend->storage = storage;
    backend->size = size;
    hal->read = nvlog_example_ram_read;
    hal->write = nvlog_example_ram_write;
    hal->user = backend;
}

#endif /* NVLOG_EXAMPLE_SUPPORT_H */

