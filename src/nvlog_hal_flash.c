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
    if (flash->erase_size == 0)                return NVLOG_ERR_PARAM;
    if (region_size % flash->erase_size != 0)  return NVLOG_ERR_PARAM;
    if (region_size < flash->erase_size)       return NVLOG_ERR_PARAM;

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
    uint32_t remaining = region_size;
    uint32_t offset    = 0;

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
