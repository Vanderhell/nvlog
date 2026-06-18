/**
 * nvlog_hal_fram.c — FRAM HAL implementation (SPI + I2C)
 */

#include "nvlog_hal_fram.h"
#include <string.h>

/* ────────────────────────────────────────────────────────────────
 * SPI FRAM — internal helpers
 * ──────────────────────────────────────────────────────────────── */

/**
 * Build address bytes into buf. Returns number of bytes written.
 * FRAM SPI address is MSB-first.
 */
static uint8_t spi_encode_addr(uint32_t addr, uint8_t addr_bytes, uint8_t *buf)
{
    if (addr_bytes == 3) {
        buf[0] = (uint8_t)(addr >> 16);
        buf[1] = (uint8_t)(addr >> 8);
        buf[2] = (uint8_t)(addr);
        return 3;
    }
    /* default: 2 bytes */
    buf[0] = (uint8_t)(addr >> 8);
    buf[1] = (uint8_t)(addr);
    return 2;
}

static int spi_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_fram_ctx_t *ctx = (nvlog_fram_ctx_t *)user;
    const nvlog_fram_spi_glue_t *g = &ctx->glue.spi;

    /* cmd + addr bytes */
    uint8_t hdr[4];
    hdr[0] = NVLOG_FRAM_SPI_OP_READ;
    uint8_t addr_len = spi_encode_addr(addr, g->addr_bytes, hdr + 1);
    uint8_t hdr_len  = 1u + addr_len;

    g->cs_set(1, g->user);
    int rc = g->transfer(hdr, NULL, hdr_len, g->user);
    if (rc == 0)
        rc = g->transfer(NULL, (uint8_t *)buf, len, g->user);
    g->cs_set(0, g->user);

    return rc;
}

static int spi_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_fram_ctx_t *ctx = (nvlog_fram_ctx_t *)user;
    const nvlog_fram_spi_glue_t *g = &ctx->glue.spi;

    /* WREN — must be sent before every write on SPI FRAM */
    uint8_t wren = NVLOG_FRAM_SPI_OP_WREN;
    g->cs_set(1, g->user);
    int rc = g->transfer(&wren, NULL, 1, g->user);
    g->cs_set(0, g->user);
    if (rc != 0)
        return rc;

    /* WRITE opcode + address */
    uint8_t hdr[4];
    hdr[0] = NVLOG_FRAM_SPI_OP_WRITE;
    uint8_t addr_len = spi_encode_addr(addr, g->addr_bytes, hdr + 1);
    uint8_t hdr_len  = 1u + addr_len;

    g->cs_set(1, g->user);
    rc = g->transfer(hdr, NULL, hdr_len, g->user);
    if (rc == 0)
        rc = g->transfer((const uint8_t *)buf, NULL, len, g->user);
    g->cs_set(0, g->user);

    return rc;
}

/* ────────────────────────────────────────────────────────────────
 * I2C FRAM — internal helpers
 *
 * MB85RC protocol:
 *   Write: START | dev_addr W | addr_hi | addr_lo | data... | STOP
 *   Read:  START | dev_addr W | addr_hi | addr_lo |
 *          RESTART | dev_addr R | data... | STOP
 *
 * The glue's i2c_write / i2c_read handle raw bytes after address phase.
 * We prepend the memory address into the first bytes of the write buffer.
 * For reads, we do a write (address-only) then a read.
 * ──────────────────────────────────────────────────────────────── */

/* Chunk size for I2C writes — keeps stack small */
#define I2C_CHUNK 64u

static int i2c_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_fram_ctx_t *ctx = (nvlog_fram_ctx_t *)user;
    const nvlog_fram_i2c_glue_t *g = &ctx->glue.i2c;

    /* send memory address */
    uint8_t addr_buf[3];
    uint8_t addr_len;
    if (g->addr_bytes == 3) {
        addr_buf[0] = (uint8_t)(addr >> 16);
        addr_buf[1] = (uint8_t)(addr >> 8);
        addr_buf[2] = (uint8_t)(addr);
        addr_len = 3;
    } else {
        addr_buf[0] = (uint8_t)(addr >> 8);
        addr_buf[1] = (uint8_t)(addr);
        addr_len = 2;
    }

    /* write address (sets internal address pointer) */
    if (g->i2c_write(g->dev_addr, addr_buf, addr_len, g->user) != 0)
        return -1;

    /* read data */
    return g->i2c_read(g->dev_addr, (uint8_t *)buf, len, g->user);
}

static int i2c_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_fram_ctx_t *ctx = (nvlog_fram_ctx_t *)user;
    const nvlog_fram_i2c_glue_t *g = &ctx->glue.i2c;

    /*
     * I2C FRAM writes: address bytes + data in one transaction.
     * We stream in I2C_CHUNK-sized pieces, advancing address each time.
     * Each piece needs its own START because we re-issue the mem address.
     *
     * Most I2C FRAM chips auto-increment address, but splitting into
     * chunks keeps the stack buffer small without dynamic allocation.
     */
    uint8_t  chunk[I2C_CHUNK + 3];  /* max addr_bytes=3 + chunk data */
    uint8_t  addr_len = g->addr_bytes == 3 ? 3u : 2u;
    uint32_t remaining = len;
    uint32_t offset    = 0;

    while (remaining > 0) {
        uint32_t n = remaining < I2C_CHUNK ? remaining : I2C_CHUNK;
        uint32_t cur_addr = addr + offset;

        if (addr_len == 3) {
            chunk[0] = (uint8_t)(cur_addr >> 16);
            chunk[1] = (uint8_t)(cur_addr >> 8);
            chunk[2] = (uint8_t)(cur_addr);
        } else {
            chunk[0] = (uint8_t)(cur_addr >> 8);
            chunk[1] = (uint8_t)(cur_addr);
        }
        memcpy(chunk + addr_len, (const uint8_t *)buf + offset, n);

        if (g->i2c_write(g->dev_addr, chunk, addr_len + n, g->user) != 0)
            return -1;

        offset    += n;
        remaining -= n;
    }

    return 0;
}

/* ────────────────────────────────────────────────────────────────
 * Public init
 * ──────────────────────────────────────────────────────────────── */

nvlog_status_t nvlog_fram_init_spi(nvlog_fram_ctx_t            *ctx,
                                    nvlog_hal_t                *hal_out,
                                    const nvlog_fram_spi_glue_t *glue)
{
    if (!ctx || !hal_out || !glue)           return NVLOG_ERR_PARAM;
    if (!glue->cs_set || !glue->transfer)   return NVLOG_ERR_PARAM;
    if (glue->addr_bytes != 2 &&
        glue->addr_bytes != 3)              return NVLOG_ERR_PARAM;

    ctx->transport = NVLOG_FRAM_SPI;
    ctx->glue.spi  = *glue;

    hal_out->read  = spi_read;
    hal_out->write = spi_write;
    hal_out->user  = ctx;

    return NVLOG_OK;
}

nvlog_status_t nvlog_fram_init_i2c(nvlog_fram_ctx_t            *ctx,
                                    nvlog_hal_t                *hal_out,
                                    const nvlog_fram_i2c_glue_t *glue)
{
    if (!ctx || !hal_out || !glue)              return NVLOG_ERR_PARAM;
    if (!glue->i2c_write || !glue->i2c_read)   return NVLOG_ERR_PARAM;
    if (glue->addr_bytes != 2 &&
        glue->addr_bytes != 3)                 return NVLOG_ERR_PARAM;

    ctx->transport = NVLOG_FRAM_I2C;
    ctx->glue.i2c  = *glue;

    hal_out->read  = i2c_read;
    hal_out->write = i2c_write;
    hal_out->user  = ctx;

    return NVLOG_OK;
}
