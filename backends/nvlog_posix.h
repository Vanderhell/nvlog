/**
 * nvlog_posix.h / nvlog_posix.c — POSIX backend (file or RAM)
 *
 * Two modes:
 *   nvlog_posix_open_file()  — backed by a real file (persistent)
 *   nvlog_posix_open_ram()   — backed by a malloc'd buffer (test/sim)
 *
 * Both simulate byte-writable NVM (like FRAM/EEPROM).
 * Power-loss simulation: nvlog_posix_inject_fail_after()
 */

#ifndef NVLOG_POSIX_H
#define NVLOG_POSIX_H

#include "nvlog.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE    *fp;           /* non-NULL if file-backed */
    uint8_t *ram;          /* non-NULL if RAM-backed */
    uint32_t size;
    int32_t  fail_after;   /* -1 = no inject; >=0 = fail after N writes */
    uint32_t write_count;
    int32_t  read_fail_after;
    uint32_t read_count;
} nvlog_posix_ctx_t;

/**
 * Open a file-backed backend.
 * File is created/truncated at 'size' bytes if it doesn't exist.
 */
int nvlog_posix_open_file(nvlog_posix_ctx_t *pctx,
                           nvlog_hal_t *hal_out,
                           const char *path,
                           uint32_t size);

/**
 * Open a RAM-backed backend.
 * Allocates 'size' bytes with malloc, zeroes the buffer.
 */
int nvlog_posix_open_ram(nvlog_posix_ctx_t *pctx,
                          nvlog_hal_t *hal_out,
                          uint32_t size);

/**
 * Inject a simulated power-loss.
 * After 'n' write calls, all subsequent writes return error.
 * Set n = -1 to disable.
 */
void nvlog_posix_inject_fail_after(nvlog_posix_ctx_t *pctx, int32_t n);

void nvlog_posix_inject_read_fail_after(nvlog_posix_ctx_t *pctx, int32_t n);

/**
 * Close and free resources.
 */
void nvlog_posix_close(nvlog_posix_ctx_t *pctx);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_POSIX_H */
