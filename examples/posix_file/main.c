#include <stdint.h>
#include <string.h>
#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

int main(void)
{
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    nvlog_ctx_t ctx;
    nvlog_iter_t it;
    nvlog_record_t rec;
    uint8_t buf[32];

    nvlog_ctx_init(&ctx);
    if (nvlog_posix_open_file(&pctx, &hal, "nvlog-example.bin", 4096u) != 0) return 1;
    if (nvlog_mount(&ctx, &hal, 4096u) != NVLOG_OK) {
        if (nvlog_format(&ctx, &hal, 4096u) != NVLOG_OK) return 1;
    }
    if (nvlog_append(&ctx, "file", 4u) != NVLOG_OK) return 1;
    if (nvlog_iter_init(&it, &ctx) != NVLOG_OK) return 1;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
        if (rec.len > sizeof(buf)) return 1;
        if (nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) != NVLOG_OK) return 1;
    }
    nvlog_posix_close(&pctx);
    return 0;
}

