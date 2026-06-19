/**
 * nvlog_hal_flash.c — flash HAL format + verify implementation
 */

#include "nvlog_hal_flash.h"
#include <string.h>

extern nvlog_status_t format_impl(nvlog_ctx_t *ctx,
                                  const nvlog_hal_t *hal,
                                  uint32_t region_size,
                                  nvlog_mode_t mode,
                                  uint8_t media_class,
                                  uint8_t program_unit,
                                  uint8_t erased_value,
                                  uint32_t geometry_key);

/* ─── nvlog_flash_format ─────────────────────────────────────── */

nvlog_status_t nvlog_flash_format(nvlog_ctx_t             *ctx,
                                   const nvlog_hal_flash_t *flash,
                                   uint32_t                 region_size)
{
    if (!ctx || !flash)                        return NVLOG_ERR_PARAM;
    if (!flash->erase)                         return NVLOG_ERR_PARAM;
    if (!flash->base.read || !flash->base.write) return NVLOG_ERR_PARAM;
    if (flash->user && flash->base.user && flash->user != flash->base.user)
        return NVLOG_ERR_PARAM;
    if (flash->erase_size == 0)                return NVLOG_ERR_PARAM;
    if (flash->prog_size == 0)                 return NVLOG_ERR_PARAM;
    if (flash->prog_size != 1u &&
        flash->prog_size != 4u &&
        flash->prog_size != 8u &&
        flash->prog_size != 32u)
        return NVLOG_ERR_PARAM;
    if (region_size % flash->erase_size != 0)  return NVLOG_ERR_PARAM;
    if (region_size < flash->erase_size)       return NVLOG_ERR_PARAM;

    ctx->media_class = NVLOG_MEDIA_CLASS_ERASE_BEFORE_WRITE;
    ctx->program_unit = (uint8_t)flash->prog_size;
    ctx->erased_value = 0xFFu;
    ctx->geometry_key = (flash->erase_size << 16) | (flash->prog_size & 0xFFFFu);

    /* erase entire region sector by sector */
    for (uint32_t off = 0; off < region_size; off += flash->erase_size) {
        if (flash->erase(off, flash->erase_size, flash->user) != 0)
            return NVLOG_ERR_IO;
    }

    return format_impl(ctx, &flash->base, region_size,
                       NVLOG_MODE_LINEAR,
                       NVLOG_MEDIA_CLASS_ERASE_BEFORE_WRITE,
                       (uint8_t)flash->prog_size,
                       0xFFu,
                       (flash->erase_size << 16) | (flash->prog_size & 0xFFFFu));
}

/* ─── nvlog_flash_verify_erased ──────────────────────────────── */

nvlog_status_t nvlog_flash_verify_erased(const nvlog_hal_flash_t *flash,
                                          uint32_t                 region_size)
{
    if (!flash || !flash->base.read || region_size == 0)
        return NVLOG_ERR_PARAM;

    uint8_t  buf[32];
    uint8_t  head[8];
    uint32_t remaining = region_size;
    uint32_t offset    = 0;

    if (region_size >= sizeof(head) &&
        flash->base.read(0, head, sizeof(head), flash->base.user) == 0) {
        uint32_t magic = (uint32_t)head[0] |
                         ((uint32_t)head[1] << 8) |
                         ((uint32_t)head[2] << 16) |
                         ((uint32_t)head[3] << 24);
        uint16_t version = (uint16_t)head[4] | ((uint16_t)head[5] << 8);
        if (magic == NVLOG_MEDIA_MAGIC && version == NVLOG_MEDIA_VERSION) {
            offset = NVLOG_REGION_HEADER_SIZE;
            if (offset > region_size)
                return NVLOG_ERR_PARAM;
            remaining = region_size - offset;
        }
    }

    while (remaining > 0) {
        uint32_t n = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (flash->base.read(offset, buf, n, flash->base.user) != 0)
            return NVLOG_ERR_IO;
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] != 0xFF)
                return NVLOG_ERR_CORRUPT;
        }
        offset    += n;
        remaining -= n;
    }
    return NVLOG_OK;
}
