/**
 * nvlog_hal_eeprom.c — I2C EEPROM HAL implementation
 *
 * Key responsibilities:
 *   1. Page-boundary-aware writes (never cross a page in one I2C tx)
 *   2. ACK polling after write (wait for tWR completion)
 *   3. Address encoding (1 or 2 bytes)
 */

#include "nvlog_hal_eeprom.h"
#include <string.h>

/* ─── internal: ACK poll ─────────────────────────────────────────── */

/**
 * Wait until EEPROM ACKs a write (internal write cycle complete).
 * Polls by sending a dummy write with dev_addr; EEPROM NACKs while busy.
 *
 * Returns 0 when ACK received, -1 on timeout.
 */
static int eeprom_ack_poll(nvlog_eeprom_ctx_t *ctx)
{
    const nvlog_eeprom_glue_t *g   = &ctx->glue;
    uint32_t                   tmo = ctx->cfg.wr_timeout_ms;
    uint32_t                   ms  = 0;

    while (ms <= tmo) {
        /* zero-byte write: just address phase, checks ACK */
        if (g->i2c_write(ctx->cfg.dev_addr, NULL, 0, g->user) == 0)
            return 0;   /* ACK received — write done */

        if (g->delay_ms)
            g->delay_ms(1, g->user);
        ms++;
    }
    return -1;  /* timeout */
}

/* ─── internal: encode mem address ──────────────────────────────── */

static uint8_t encode_addr(uint32_t addr, uint8_t addr_bytes, uint8_t *buf)
{
    if (addr_bytes == 2) {
        buf[0] = (uint8_t)(addr >> 8);
        buf[1] = (uint8_t)(addr);
        return 2;
    }
    buf[0] = (uint8_t)(addr);
    return 1;
}

/* ─── HAL read ───────────────────────────────────────────────────── */

static int eeprom_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_eeprom_ctx_t *ctx = (nvlog_eeprom_ctx_t *)user;
    const nvlog_eeprom_glue_t *g = &ctx->glue;

    uint8_t addr_buf[2];
    uint8_t addr_len = encode_addr(addr, ctx->cfg.addr_bytes, addr_buf);

    return g->i2c_read(ctx->cfg.dev_addr,
                       addr_buf, addr_len,
                       (uint8_t *)buf, len,
                       g->user);
}

/* ─── HAL write (page-safe) ──────────────────────────────────────── */

/**
 * EEPROM writes must not cross page boundaries.
 *
 * Strategy: split each write into page-aligned chunks.
 * Each chunk is one I2C transaction followed by ACK polling.
 *
 * Example: page_size=64, addr=60, len=10
 *   chunk 1: addr=60, len=4  (fills page 0: bytes 60..63)
 *   chunk 2: addr=64, len=6  (fills page 1: bytes 64..69)
 */
static int eeprom_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_eeprom_ctx_t  *ctx  = (nvlog_eeprom_ctx_t *)user;
    const nvlog_eeprom_glue_t *g = &ctx->glue;
    uint16_t             page  = ctx->cfg.page_size;

    /* scratch buffer: addr_bytes (max 2) + page_size bytes */
    uint8_t  chunk[2 + 128];  /* covers up to 128B page (AT24C512) */
    uint32_t offset    = 0;
    uint32_t remaining = len;

    while (remaining > 0) {
        uint32_t cur_addr    = addr + offset;
        /* bytes left until next page boundary */
        uint32_t page_offset = cur_addr % page;
        uint32_t space       = page - page_offset;
        uint32_t n           = remaining < space ? remaining : space;

        uint8_t addr_len = encode_addr(cur_addr, ctx->cfg.addr_bytes, chunk);
        memcpy(chunk + addr_len, (const uint8_t *)buf + offset, n);

        if (g->i2c_write(ctx->cfg.dev_addr, chunk, addr_len + n, g->user) != 0)
            return -1;

        /* wait for internal write to complete */
        if (eeprom_ack_poll(ctx) != 0)
            return -1;

        offset    += n;
        remaining -= n;
    }

    return 0;
}

/* ─── init ───────────────────────────────────────────────────────── */

nvlog_status_t nvlog_eeprom_init(nvlog_eeprom_ctx_t        *ctx,
                                  nvlog_hal_t               *hal_out,
                                  const nvlog_eeprom_glue_t *glue,
                                  const nvlog_eeprom_cfg_t  *cfg)
{
    if (!ctx || !hal_out || !glue || !cfg)          return NVLOG_ERR_PARAM;
    if (!glue->i2c_write || !glue->i2c_read)        return NVLOG_ERR_PARAM;
    if (cfg->page_size == 0 || cfg->capacity == 0)  return NVLOG_ERR_PARAM;
    if (cfg->addr_bytes != 1 && cfg->addr_bytes != 2) return NVLOG_ERR_PARAM;

    ctx->glue = *glue;
    ctx->cfg  = *cfg;

    hal_out->read  = eeprom_read;
    hal_out->write = eeprom_write;
    hal_out->user  = ctx;

    return NVLOG_OK;
}
