/**
 * nvlog_hal_flash.c — flash HAL format + verify implementation
 */

#include "nvlog_hal_flash.h"
#include <string.h>

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

    /* delegate to nvlog_format() using the embedded base HAL */
    return nvlog_format(ctx, &flash->base, region_size);
}

/* ─── nvlog_flash_verify_erased ──────────────────────────────── */

nvlog_status_t nvlog_flash_verify_erased(const nvlog_hal_flash_t *flash,
                                          uint32_t                 region_size)
{
    if (!flash || !flash->base.read || region_size == 0)
        return NVLOG_ERR_PARAM;

    uint8_t  buf[32];
    uint32_t remaining = 0;
    uint32_t offset    = 0;

    if (region_size <= NVLOG_REGION_HEADER_SIZE)
        return NVLOG_OK;

    remaining = region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
    offset    = (uint32_t)NVLOG_REGION_HEADER_SIZE;

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
