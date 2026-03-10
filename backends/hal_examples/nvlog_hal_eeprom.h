/**
 * nvlog_hal_eeprom.h — I2C EEPROM HAL for nvlog
 *
 * Supports AT24Cxx family (Microchip/Atmel):
 *   AT24C32   (4KB,  32B page)
 *   AT24C64   (8KB,  32B page)
 *   AT24C128  (16KB, 64B page)
 *   AT24C256  (32KB, 64B page)
 *   AT24C512  (64KB, 128B page)
 *
 * Also compatible with:
 *   ST M24C32/64/128/256
 *   ON CAT24Cxx
 *
 * ⚠️  EEPROM vs FRAM — important difference:
 *
 *   EEPROM has limited write endurance (~1M cycles) and a page write buffer.
 *   Writes that cross a page boundary wrap within the page — you MUST stay
 *   within a single page per I2C transaction, or use the page-aligned write
 *   helper provided here (nvlog_eeprom_write_safe).
 *
 *   FRAM has no page limitation and effectively unlimited endurance.
 *   If you have a choice: prefer FRAM for nvlog.
 *
 * Write timing:
 *   After each write transaction, EEPROM needs up to 5ms (tWR) before
 *   accepting a new write. This HAL polls ACK (acknowledge polling)
 *   to detect write completion — requires the glue i2c_write to return
 *   non-zero on NACK (busy).
 *
 *   ACK polling timeout: configurable via nvlog_eeprom_cfg_t.wr_timeout_ms
 */

#ifndef NVLOG_HAL_EEPROM_H
#define NVLOG_HAL_EEPROM_H

#include "nvlog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── MCU glue ───────────────────────────────────────────────────── */

typedef struct {
    /**
     * i2c_write — send buf[0..len-1] to dev_addr.
     *   Returns 0 on ACK, non-zero on NACK or error.
     *   Used both for data writes and ACK polling (len=0 or len=addr_bytes).
     */
    int  (*i2c_write)(uint8_t dev_addr, const uint8_t *buf, uint32_t len, void *user);

    /**
     * i2c_read — send memory address then read len bytes.
     *   Glue must handle: write(dev_addr, addr_buf, addr_len) +
     *                     repeated-start + read(dev_addr, buf, len)
     *   Returns 0 on success.
     */
    int  (*i2c_read) (uint8_t dev_addr, const uint8_t *addr_buf,
                      uint8_t addr_len, uint8_t *buf, uint32_t len, void *user);

    /**
     * delay_ms — busy-wait or RTOS delay, used for ACK polling backoff.
     *   Can be NULL if ACK polling returns quickly (fast I2C + fast MCU).
     *   If NULL: HAL spins on i2c_write without delay.
     */
    void (*delay_ms) (uint32_t ms, void *user);

    void *user;
} nvlog_eeprom_glue_t;

/* ─── EEPROM config ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  dev_addr;       /* 7-bit I2C address (0x50..0x57) */
    uint8_t  addr_bytes;     /* 1 for <=2KB devices, 2 for >2KB */
    uint16_t page_size;      /* write page size in bytes (32, 64, 128...) */
    uint32_t capacity;       /* total bytes (4096, 8192, ...) */
    uint32_t wr_timeout_ms;  /* ACK poll timeout (5ms typical, 10ms safe) */
} nvlog_eeprom_cfg_t;

/* Preset configs for common devices */
#define NVLOG_EEPROM_AT24C32  { .dev_addr=0x50, .addr_bytes=2, .page_size=32,  .capacity=4096,  .wr_timeout_ms=10 }
#define NVLOG_EEPROM_AT24C64  { .dev_addr=0x50, .addr_bytes=2, .page_size=32,  .capacity=8192,  .wr_timeout_ms=10 }
#define NVLOG_EEPROM_AT24C128 { .dev_addr=0x50, .addr_bytes=2, .page_size=64,  .capacity=16384, .wr_timeout_ms=10 }
#define NVLOG_EEPROM_AT24C256 { .dev_addr=0x50, .addr_bytes=2, .page_size=64,  .capacity=32768, .wr_timeout_ms=10 }
#define NVLOG_EEPROM_AT24C512 { .dev_addr=0x50, .addr_bytes=2, .page_size=128, .capacity=65536, .wr_timeout_ms=10 }

/* ─── HAL context ────────────────────────────────────────────────── */

typedef struct {
    nvlog_eeprom_glue_t glue;
    nvlog_eeprom_cfg_t  cfg;
} nvlog_eeprom_ctx_t;

/* ─── init ───────────────────────────────────────────────────────── */

/**
 * nvlog_eeprom_init() — configure EEPROM backend.
 *
 * ctx must remain valid for the nvlog session lifetime.
 */
nvlog_status_t nvlog_eeprom_init(nvlog_eeprom_ctx_t        *ctx,
                                  nvlog_hal_t               *hal_out,
                                  const nvlog_eeprom_glue_t *glue,
                                  const nvlog_eeprom_cfg_t  *cfg);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_HAL_EEPROM_H */
