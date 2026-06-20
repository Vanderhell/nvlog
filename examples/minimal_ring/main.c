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
    uint8_t payload[16];

    memset(storage, 0xFF, sizeof(storage));
    nvlog_example_ram_bind(&hal, &backend, storage, (uint32_t)sizeof(storage));
    nvlog_ctx_init(&ctx);

    if (nvlog_ring_format(&ctx, &hal, (uint32_t)sizeof(storage)) != NVLOG_OK) return 1;
    for (uint32_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    if (nvlog_append(&ctx, payload, sizeof(payload)) != NVLOG_OK) return 1;
    if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return 1;
    return nvlog_iter_next(&it, &rec) == NVLOG_OK ? 0 : 1;
}

