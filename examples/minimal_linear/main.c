#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../common/nvlog_example_support.h"

int main(void)
{
    uint8_t storage[4096u];
    nvlog_example_ram_backend_t backend;
    nvlog_hal_t hal;
    nvlog_ctx_t ctx;
    nvlog_iter_t it;
    nvlog_record_t rec;
    uint8_t buf[32];

    memset(storage, 0xFF, sizeof(storage));
    nvlog_example_ram_bind(&hal, &backend, storage, (uint32_t)sizeof(storage));
    nvlog_ctx_init(&ctx);

    if (nvlog_format(&ctx, &hal, (uint32_t)sizeof(storage)) != NVLOG_OK) return 1;
    if (nvlog_append(&ctx, "boot", 4u) != NVLOG_OK) return 1;
    if (nvlog_append(&ctx, "ready", 5u) != NVLOG_OK) return 1;
    if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return 1;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
        if (rec.len > sizeof(buf)) return 1;
        if (nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) != NVLOG_OK) return 1;
        fwrite(buf, 1, rec.len, stdout);
        fputc('\n', stdout);
    }
    return 0;
}

