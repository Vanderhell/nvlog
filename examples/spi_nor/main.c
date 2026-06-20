#include <stdint.h>
#include <string.h>
#include "../common/nvlog_example_support.h"

int main(void)
{
    uint8_t storage[16384u];
    nvlog_example_ram_backend_t backend;
    nvlog_hal_t hal;
    nvlog_ctx_t ctx;

    memset(storage, 0xFF, sizeof(storage));
    nvlog_example_ram_bind(&hal, &backend, storage, (uint32_t)sizeof(storage));
    nvlog_ctx_init(&ctx);
    return nvlog_format(&ctx, &hal, (uint32_t)sizeof(storage)) == NVLOG_OK ? 0 : 1;
}

