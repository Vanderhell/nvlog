#include "nvlog_espidf_partition.h"

#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
struct esp_partition_t {
    int unused;
};
extern esp_err_t esp_partition_read(const esp_partition_t *partition,
                                    size_t offset, void *dst, size_t size);
extern esp_err_t esp_partition_write(const esp_partition_t *partition,
                                     size_t offset, const void *src, size_t size);
extern esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                           size_t offset, size_t size);
#endif

#include <string.h>

static int espidf_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_espidf_partition_ctx_t *ctx = (nvlog_espidf_partition_ctx_t *)user;
    if (!ctx || !ctx->partition) return -1;
    if (addr > ctx->capacity || len > ctx->capacity - addr) return -1;
    return esp_partition_read(ctx->partition, (size_t)addr, buf, (size_t)len) == ESP_OK ? 0 : -1;
}

static int espidf_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_espidf_partition_ctx_t *ctx = (nvlog_espidf_partition_ctx_t *)user;
    if (!ctx || !ctx->partition) return -1;
    if (addr > ctx->capacity || len > ctx->capacity - addr) return -1;
    return esp_partition_write(ctx->partition, (size_t)addr, buf, (size_t)len) == ESP_OK ? 0 : -1;
}

static int espidf_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_espidf_partition_ctx_t *ctx = (nvlog_espidf_partition_ctx_t *)user;
    if (!ctx || !ctx->partition) return -1;
    if (ctx->erase_size == 0) return -1;
    if (addr % ctx->erase_size != 0 || len % ctx->erase_size != 0) return -1;
    if (addr > ctx->capacity || len > ctx->capacity - addr) return -1;
    return esp_partition_erase_range(ctx->partition, (size_t)addr, (size_t)len) == ESP_OK ? 0 : -1;
}

nvlog_status_t nvlog_espidf_partition_init(nvlog_espidf_partition_ctx_t *ctx,
                                           nvlog_hal_flash_t *flash_out,
                                           const esp_partition_t *partition,
                                           uint32_t capacity,
                                           uint32_t erase_size,
                                           uint32_t program_size)
{
    if (!ctx || !flash_out || !partition) return NVLOG_ERR_PARAM;
    if (capacity == 0 || erase_size == 0 || program_size == 0) return NVLOG_ERR_PARAM;
    if (capacity % erase_size != 0) return NVLOG_ERR_PARAM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->partition = partition;
    ctx->capacity = capacity;
    ctx->erase_size = erase_size;
    ctx->program_size = program_size;

    flash_out->base.read = espidf_read;
    flash_out->base.write = espidf_write;
    flash_out->base.user = ctx;
    flash_out->erase = espidf_erase;
    flash_out->erase_size = erase_size;
    flash_out->prog_size = program_size;
    flash_out->user = ctx;
    return NVLOG_OK;
}
