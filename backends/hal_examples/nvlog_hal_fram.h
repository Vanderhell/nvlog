/**
 * nvlog_hal_fram.h — Generic FRAM HAL for nvlog
 *
 * Supports two transport variants selectable at init time:
 *
 *   A) SPI FRAM  — Fujitsu MB85RS series (MB85RS64, MB85RS256, MB85RS1MT...)
 *                  Cypress/Infineon FM25 series
 *                  Operates with standard SPI READ/WRITE opcodes
 *
 *   B) I2C FRAM  — Fujitsu MB85RC series (MB85RC64, MB85RC256V, MB85RC512T...)
 *                  Cypress/Infineon FM24 series
 *                  16-bit address, 7-bit I2C device address
 *
 * Both variants present an identical nvlog_hal_t to the nvlog core.
 * No dynamic allocation. HAL state lives in nvlog_fram_ctx_t (caller provides).
 *
 * MCU glue required (user implements):
 *   For SPI: spi_transfer, cs_set
 *   For I2C: i2c_write, i2c_read
 *
 * Tested against:
 *   MB85RS256B  (32KB SPI FRAM)
 *   MB85RC256V  (32KB I2C FRAM)
 *   Simulated via nvlog_posix RAM backend in unit tests
 */

#ifndef NVLOG_HAL_FRAM_H
#define NVLOG_HAL_FRAM_H

#include "nvlog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── SPI FRAM opcodes (JEDEC standard, all MB85RS/FM25 compatible) ── */

#define NVLOG_FRAM_SPI_OP_WREN   0x06u  /* Write Enable Latch */
#define NVLOG_FRAM_SPI_OP_WRDI   0x04u  /* Write Disable */
#define NVLOG_FRAM_SPI_OP_RDSR   0x05u  /* Read Status Register */
#define NVLOG_FRAM_SPI_OP_WRSR   0x01u  /* Write Status Register */
#define NVLOG_FRAM_SPI_OP_READ   0x03u  /* Read Memory */
#define NVLOG_FRAM_SPI_OP_WRITE  0x02u  /* Write Memory */
#define NVLOG_FRAM_SPI_OP_RDID   0x9Fu  /* Read Device ID */
#define NVLOG_FRAM_SPI_OP_SLEEP  0xB9u  /* Enter Sleep Mode */

/* ─── transport type ─────────────────────────────────────────────── */

typedef enum {
    NVLOG_FRAM_SPI = 0,
    NVLOG_FRAM_I2C = 1,
} nvlog_fram_transport_t;

/* ─── MCU glue callbacks ─────────────────────────────────────────── */

/**
 * SPI glue:
 *
 *   cs_set(state, user)     — drive CS pin: state=1 assert, state=0 deassert
 *   spi_transfer(tx, rx, len, user) — full-duplex transfer, len bytes
 *                             tx or rx may be NULL (half-duplex)
 *
 * Timing requirements (FRAM is fast):
 *   - SPI mode 0 or 3 (CPOL=0/CPHA=0 or CPOL=1/CPHA=1)
 *   - No delays needed between CS assert and first clock
 *   - Max freq: MB85RS256 = 40 MHz, MB85RS1MT = 40 MHz
 */
typedef struct {
    void (*cs_set)     (int assert, void *user);
    int  (*transfer)   (const uint8_t *tx, uint8_t *rx, uint32_t len, void *user);
    void *user;
    uint8_t addr_bytes;  /* 2 for <=256KB, 3 for >256KB (MB85RS1MT etc.) */
} nvlog_fram_spi_glue_t;

/**
 * I2C glue:
 *
 *   i2c_write(dev_addr, buf, len, user) — write len bytes, return 0 on ACK
 *   i2c_read (dev_addr, buf, len, user) — read  len bytes, return 0 on ACK
 *
 *   dev_addr — 7-bit I2C address (0x50..0x57 for MB85RC, set by A2:A0 pins)
 *
 * Note: MB85RC uses combined write (addr+data) for writes,
 *       and write-then-repeated-start-read for reads.
 *       This is handled inside the HAL — glue only needs raw write/read.
 */
typedef struct {
    int  (*i2c_write)(uint8_t dev_addr, const uint8_t *buf, uint32_t len, void *user);
    int  (*i2c_read) (uint8_t dev_addr, uint8_t *buf, uint32_t len, void *user);
    void *user;
    uint8_t dev_addr;    /* 7-bit I2C address */
    uint8_t addr_bytes;  /* 2 for MB85RC64..512, 3 for MB85RC1MT */
} nvlog_fram_i2c_glue_t;

/* ─── HAL context (opaque to caller, no malloc) ──────────────────── */

typedef struct {
    nvlog_fram_transport_t transport;
    union {
        nvlog_fram_spi_glue_t spi;
        nvlog_fram_i2c_glue_t i2c;
    } glue;
} nvlog_fram_ctx_t;

/* ─── init functions ─────────────────────────────────────────────── */

/**
 * nvlog_fram_init_spi() — configure SPI FRAM backend.
 *
 * Fills hal_out with read/write callbacks backed by ctx.
 * ctx must remain valid for the lifetime of the nvlog session.
 */
nvlog_status_t nvlog_fram_init_spi(nvlog_fram_ctx_t       *ctx,
                                    nvlog_hal_t            *hal_out,
                                    const nvlog_fram_spi_glue_t *glue);

/**
 * nvlog_fram_init_i2c() — configure I2C FRAM backend.
 */
nvlog_status_t nvlog_fram_init_i2c(nvlog_fram_ctx_t       *ctx,
                                    nvlog_hal_t            *hal_out,
                                    const nvlog_fram_i2c_glue_t *glue);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_HAL_FRAM_H */
