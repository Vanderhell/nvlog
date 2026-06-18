/**
 * nvlog_hal_flash.h - Flash HAL extension for nvlog
 *
 * This header defines a flash-specific extension of the core byte-writable
 * HAL. The core API owns read/write semantics; the flash extension adds erase
 * geometry and a dedicated format helper for erase-before-write media.
 *
 * The file intentionally documents flash-specific constraints separately from
 * the portable core so that unsupported media combinations can be rejected by
 * the implementation instead of being inferred from comments.
 */

#ifndef NVLOG_HAL_FLASH_H
#define NVLOG_HAL_FLASH_H

#include "nvlog.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nvlog_hal_t  base;
    int        (*erase)(uint32_t addr, uint32_t len, void *user);
    uint32_t     erase_size;
    uint32_t     prog_size;
    void        *user;
} nvlog_hal_flash_t;

nvlog_status_t nvlog_flash_format(nvlog_ctx_t *ctx,
                                  const nvlog_hal_flash_t *flash,
                                  uint32_t region_size);

nvlog_status_t nvlog_flash_verify_erased(const nvlog_hal_flash_t *flash,
                                         uint32_t region_size);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_HAL_FLASH_H */
