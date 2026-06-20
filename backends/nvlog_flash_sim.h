/**
 * nvlog_flash_sim.h / nvlog_flash_sim.c
 *
 * NOR flash simulator for host-side testing.
 *
 * Enforces real NOR flash physics:
 *   - initial state: all bytes 0xFF
 *   - write: bits can only change 1→0 (never 0→1 without erase)
 *   - erase: sets entire sector to 0xFF
 *   - violation detection: write attempt of 0→1 triggers fault counter
 *   - power-loss injection: same as nvlog_posix
 *   - erase counter: tracks per-sector erase cycles
 */

#ifndef NVLOG_FLASH_SIM_H
#define NVLOG_FLASH_SIM_H

#include "nvlog_hal_flash.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  *mem;
    uint32_t  size;
    uint32_t  sector_size;
    uint32_t  program_size;
    uint32_t  program_alignment;
    uint32_t  max_transfer;
    uint8_t   erased_value;

    /* physics violation tracking */
    uint32_t  bit_flip_violations; /* write 0→1 without erase */

    /* per-sector erase counters (up to 256 sectors) */
    uint32_t  erase_counts[256];
    uint32_t  num_sectors;

    /* power-loss injection */
    int32_t   fail_after_write;   /* -1 = disabled */
    uint32_t  write_count;
    int32_t   fail_during_write;  /* -1 = disabled, otherwise partial byte count */
    int32_t   fail_after_erase;   /* -1 = disabled */
    uint32_t  erase_count_global;
    int32_t   fail_during_erase;  /* -1 = disabled, otherwise partial byte count */
} nvlog_flash_sim_ctx_t;

typedef struct {
    uint32_t capacity;
    uint8_t  erased_value;
    uint32_t erase_unit;
    uint32_t program_unit;
    uint32_t program_alignment;
    uint32_t max_transfer;
    uint32_t sector_size;
} nvlog_flash_sim_cfg_t;

/**
 * Open RAM-backed NOR flash simulator.
 * sector_size: typically 4096 (NOR) or 2048 (STM32L4 internal flash)
 */
int nvlog_flash_sim_open(nvlog_flash_sim_ctx_t *sim,
                          nvlog_hal_flash_t     *flash_out,
                          uint32_t               size,
                          uint32_t               sector_size);

int nvlog_flash_sim_open_cfg(nvlog_flash_sim_ctx_t *sim,
                             nvlog_hal_flash_t     *flash_out,
                             const nvlog_flash_sim_cfg_t *cfg);

/** Inject failure: writes fail after n write calls (-1 = disable) */
void nvlog_flash_sim_inject_write_fail(nvlog_flash_sim_ctx_t *sim, int32_t n);

/** Inject failure: writes truncate after n bytes in the next write call */
void nvlog_flash_sim_inject_write_partial(nvlog_flash_sim_ctx_t *sim, int32_t bytes);

/** Inject failure: erases fail after n erase calls (-1 = disable) */
void nvlog_flash_sim_inject_erase_fail(nvlog_flash_sim_ctx_t *sim, int32_t n);

/** Inject failure: erases truncate after n bytes in the next erase call */
void nvlog_flash_sim_inject_erase_partial(nvlog_flash_sim_ctx_t *sim, int32_t bytes);

/** Reset sim state (re-erase all sectors, clear counters) */
void nvlog_flash_sim_reset(nvlog_flash_sim_ctx_t *sim);

void nvlog_flash_sim_close(nvlog_flash_sim_ctx_t *sim);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_FLASH_SIM_H */
