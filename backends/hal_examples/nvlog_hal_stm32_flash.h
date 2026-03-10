/**
 * nvlog_hal_stm32_flash.h — STM32 internal flash HAL for nvlog (v0.3)
 *
 * Supports three STM32 flash controller families, selected at compile time
 * via a define (or auto-detected from CMSIS device header):
 *
 *   NVLOG_STM32_FLASH_F4   — STM32F4xx (F401, F407, F429...)
 *   NVLOG_STM32_FLASH_L4   — STM32L4xx (L432, L476, L496...)
 *   NVLOG_STM32_FLASH_H7   — STM32H7xx (H743, H753, H750...)
 *
 * ─── Key differences between families ──────────────────────────
 *
 *  Family  │ prog_size  │ erase unit          │ erase time
 *  ────────┼────────────┼─────────────────────┼───────────
 *  F4      │ 1/2/4B     │ sector (16-128KB)   │ ~1s (128KB)
 *  L4      │ 8B         │ page (2KB)          │ ~25ms
 *  H7      │ 32B        │ sector (128KB)      │ ~4s
 *
 *  STM32F4: sector sizes are non-uniform (16/16/16/16/64/128KB for F407)
 *  STM32L4: uniform 2KB pages — much better granularity for logging
 *  STM32H7: large sectors, high write throughput, requires 32B alignment
 *
 * ─── Usage recommendation ───────────────────────────────────────
 *
 *  For nvlog, STM32L4 is the best fit:
 *   - 2KB page erase = minimal wasted space
 *   - 8B double-word write = reasonable overhead
 *   - Up to 10K erase cycles per page
 *
 *  STM32F4 is workable but wastes space (smallest sector = 16KB).
 *  STM32H7 is only justified if you need fast bulk logging.
 *
 * ─── MCU glue ───────────────────────────────────────────────────
 *
 *  Unlike SPI HALs, this HAL uses direct register access — no glue
 *  callbacks needed. You provide the base address and size of the
 *  flash region allocated for nvlog.
 *
 *  IMPORTANT: the nvlog region must be in the application flash area,
 *  NOT overlapping the bootloader or application code.
 *  Typical placement: last N sectors/pages of flash.
 *
 * ─── Linker script integration ──────────────────────────────────
 *
 *  Reserve flash for nvlog in your .ld file:
 *
 *    MEMORY {
 *      FLASH_APP  (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
 *      FLASH_LOG  (rw)  : ORIGIN = 0x08040000, LENGTH = 64K   <- nvlog
 *      RAM        (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
 *    }
 *
 *  Then in nvlog init:
 *    #define NVLOG_FLASH_BASE 0x08040000
 *    #define NVLOG_FLASH_SIZE (64 * 1024)
 */

#ifndef NVLOG_HAL_STM32_FLASH_H
#define NVLOG_HAL_STM32_FLASH_H

#include "nvlog_hal_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── family selection ───────────────────────────────────────── */

#if !defined(NVLOG_STM32_FLASH_F4) && \
    !defined(NVLOG_STM32_FLASH_L4) && \
    !defined(NVLOG_STM32_FLASH_H7)
/* Auto-detect from CMSIS if available */
#  if defined(STM32F4)
#    define NVLOG_STM32_FLASH_F4
#  elif defined(STM32L4)
#    define NVLOG_STM32_FLASH_L4
#  elif defined(STM32H7)
#    define NVLOG_STM32_FLASH_H7
#  else
#    error "Define NVLOG_STM32_FLASH_F4, _L4, or _H7 before including this header"
#  endif
#endif

/* ─── geometry constants per family ─────────────────────────── */

#if defined(NVLOG_STM32_FLASH_F4)
#  define NVLOG_STM32_PROG_SIZE    4u         /* bytes (PSIZE=10, 32-bit) */
#  define NVLOG_STM32_ERASE_MIN    (16*1024u) /* smallest sector = 16KB */

#elif defined(NVLOG_STM32_FLASH_L4)
#  define NVLOG_STM32_PROG_SIZE    8u         /* bytes (double-word) */
#  define NVLOG_STM32_ERASE_MIN    2048u      /* page = 2KB */

#elif defined(NVLOG_STM32_FLASH_H7)
#  define NVLOG_STM32_PROG_SIZE    32u        /* bytes (flash word) */
#  define NVLOG_STM32_ERASE_MIN    (128*1024u)/* sector = 128KB */
#endif

/* ─── HAL context ────────────────────────────────────────────── */

typedef struct {
    uint32_t base_addr;    /* absolute flash address of nvlog region start */
    uint32_t region_size;  /* must be multiple of NVLOG_STM32_ERASE_MIN */
} nvlog_stm32_flash_ctx_t;

/* ─── init ───────────────────────────────────────────────────── */

/**
 * nvlog_stm32_flash_init() — configure STM32 internal flash backend.
 *
 * base_addr: absolute flash address (e.g. 0x08040000)
 * region_size: must be aligned to erase page/sector size
 *
 * After init, call nvlog_flash_format() (first time) or
 * nvlog_mount() (subsequent boots) using flash_out.
 *
 * Note: flash_out->base.user and flash_out->user both point to ctx.
 */
nvlog_status_t nvlog_stm32_flash_init(nvlog_stm32_flash_ctx_t *ctx,
                                       nvlog_hal_flash_t       *flash_out,
                                       uint32_t                 base_addr,
                                       uint32_t                 region_size);

/**
 * nvlog_stm32_flash_unlock() — unlock flash for writing.
 *
 * Must be called before nvlog_flash_format() or nvlog_append().
 * Writes the KEYR sequence to unlock the flash controller.
 * Call nvlog_stm32_flash_lock() when done to re-protect.
 */
nvlog_status_t nvlog_stm32_flash_unlock(void);

/**
 * nvlog_stm32_flash_lock() — re-lock flash controller.
 * Call after all writes are complete (e.g. after session ends).
 */
void nvlog_stm32_flash_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_HAL_STM32_FLASH_H */
