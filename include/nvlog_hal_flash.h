/**
 * nvlog_hal_flash.h — Flash HAL extension for nvlog (v0.3)
 *
 * WHY a separate HAL for flash
 * ────────────────────────────
 * NOR flash is fundamentally different from FRAM/EEPROM:
 *
 *   FRAM/EEPROM          │  NOR flash
 *   ─────────────────────┼───────────────────────────────
 *   byte-write anywhere  │  erase-before-write (sector)
 *   no alignment needed  │  write must be prog_size aligned
 *   ~unlimited endurance │  ~100K erase cycles per sector
 *   write = change bits  │  write: 1→0 only; erase: 0→1 (whole sector)
 *
 * The nvlog_hal_t (v0.1) has no erase callback — intentional.
 * This file extends it with erase + flash-specific constraints.
 *
 * Design decision for linear mode (v0.3)
 * ───────────────────────────────────────
 * Linear mode makes flash manageable:
 *
 *   nvlog_flash_format() — erases all sectors in region ONCE
 *   nvlog_append()       — writes into pre-erased space (no erase needed)
 *   nvlog_mount()        — unchanged from v0.1 core
 *
 * Result: erase only happens on explicit format. During normal operation
 * the region is append-only into erased space → power-loss semantics
 * identical to FRAM (CRC is still the commit point).
 *
 * Constraints enforced by this layer:
 *   1. region_size must be a multiple of erase_size
 *   2. record writes are split at prog_size boundaries
 *   3. region must be erased (0xFF) before first append
 *
 * What is NOT handled here (explicit non-goals):
 *   - Wear leveling
 *   - Bad block management
 *   - Ring mode on flash (v0.4+ and only after careful design)
 *   - OTA / dual-bank schemes
 */

#ifndef NVLOG_HAL_FLASH_H
#define NVLOG_HAL_FLASH_H

#include "nvlog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── flash-extended HAL ─────────────────────────────────────── */

/**
 * nvlog_hal_flash_t — superset of nvlog_hal_t for flash backends.
 *
 * The embedded nvlog_hal_t is passed to nvlog core functions.
 * The erase callback is only used by nvlog_flash_format().
 *
 * Fields:
 *   base       — standard read/write callbacks (compatible with nvlog core)
 *   erase      — erase one or more sectors; addr and len must be
 *                erase_size-aligned. Returns 0 on success.
 *   erase_size — sector erase granularity in bytes (4096 typical for NOR)
 *   prog_size  — minimum write unit in bytes:
 *                  W25Qxx SPI NOR:    1 (byte-program) or 256 (page)
 *                  STM32F4 internal:  1, 2, or 4 (PSIZE setting)
 *                  STM32H7 internal:  32 (flash word)
 *                  STM32L4 internal:  8  (double-word)
 *   user       — passed back to all callbacks
 */
typedef struct {
    nvlog_hal_t  base;        /* MUST be first — cast-compatible with nvlog_hal_t */

    int        (*erase)(uint32_t addr, uint32_t len, void *user);
    uint32_t     erase_size;
    uint32_t     prog_size;
    void        *user;        /* separate from base.user for clarity */
} nvlog_hal_flash_t;

/* ─── flash format ───────────────────────────────────────────── */

/**
 * nvlog_flash_format() — erase region and initialise nvlog header.
 *
 * Replaces nvlog_format() for flash backends.
 * Erases all sectors in [0, region_size), then writes region header.
 *
 * Requirements:
 *   - region_size % flash->erase_size == 0
 *   - flash->erase, flash->base.write must be non-NULL
 *
 * ⚠️  This consumes one erase cycle per sector.
 *     ~100K cycles typical for NOR flash.
 *     Do not call unnecessarily.
 */
nvlog_status_t nvlog_flash_format(nvlog_ctx_t           *ctx,
                                   const nvlog_hal_flash_t *flash,
                                   uint32_t               region_size);

/**
 * nvlog_flash_verify_erased() — check that region is 0xFF.
 *
 * Useful after format to detect marginal erase or verify fresh chip.
 * Returns NVLOG_OK if all bytes are 0xFF, NVLOG_ERR_CORRUPT otherwise.
 */
nvlog_status_t nvlog_flash_verify_erased(const nvlog_hal_flash_t *flash,
                                          uint32_t                region_size);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_HAL_FLASH_H */
