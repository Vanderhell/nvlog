/**
 * nvlog_hal_nor_spi.c — W25Qxx / JEDEC SPI NOR implementation
 */

#include "nvlog_hal_nor_spi.h"
#include <string.h>

/* ─── internal: address encoding ────────────────────────────── */

static uint8_t encode_addr(uint32_t addr, uint8_t addr_bytes, uint8_t *buf)
{
    if (addr_bytes == 4) {
        buf[0] = (uint8_t)(addr >> 24);
        buf[1] = (uint8_t)(addr >> 16);
        buf[2] = (uint8_t)(addr >> 8);
        buf[3] = (uint8_t)(addr);
        return 4;
    }
    /* 3-byte default */
    buf[0] = (uint8_t)(addr >> 16);
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)(addr);
    return 3;
}

/* ─── internal: SPI helpers ──────────────────────────────────── */

static inline void cs_assert  (nvlog_nor_spi_ctx_t *c) { c->glue.cs_set(1, c->glue.user); }
static inline void cs_deassert(nvlog_nor_spi_ctx_t *c) { c->glue.cs_set(0, c->glue.user); }

static int spi_tx(nvlog_nor_spi_ctx_t *c, const uint8_t *tx, uint32_t len)
{
    return c->glue.transfer(tx, NULL, len, c->glue.user);
}

static int spi_rx(nvlog_nor_spi_ctx_t *c, uint8_t *rx, uint32_t len)
{
    return c->glue.transfer(NULL, rx, len, c->glue.user);
}

/* ─── internal: Write Enable ─────────────────────────────────── */

static int nor_write_enable(nvlog_nor_spi_ctx_t *c)
{
    uint8_t cmd = NOR_OP_WREN;
    cs_assert(c);
    int rc = spi_tx(c, &cmd, 1);
    cs_deassert(c);
    return rc;
}

/* ─── internal: WIP poll ─────────────────────────────────────── */

/**
 * Poll Status Register bit 0 (WIP) until clear or timeout.
 * Called after page program and sector erase.
 */
static int nor_wait_ready(nvlog_nor_spi_ctx_t *c)
{
    uint32_t elapsed = 0;
    uint32_t timeout = c->glue.busy_timeout_ms;

    while (elapsed <= timeout) {
        uint8_t cmd = NOR_OP_RDSR;
        uint8_t sr  = 0;

        cs_assert(c);
        spi_tx(c, &cmd, 1);
        spi_rx(c, &sr, 1);
        cs_deassert(c);

        if (!(sr & NOR_SR_WIP))
            return 0;   /* not busy */

        if (c->glue.delay_ms)
            c->glue.delay_ms(1, c->glue.user);
        elapsed++;
    }
    return -1;  /* timeout */
}

/* ─── HAL read callback ──────────────────────────────────────── */

static int nor_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_nor_spi_ctx_t *c = (nvlog_nor_spi_ctx_t *)user;

    uint8_t  hdr[5];   /* opcode + up to 4 addr bytes */
    hdr[0] = NOR_OP_READ;
    uint8_t  addr_len = encode_addr(addr, c->glue.addr_bytes, hdr + 1);
    uint8_t  hdr_len  = 1u + addr_len;

    cs_assert(c);
    int rc = spi_tx(c, hdr, hdr_len);
    if (rc == 0)
        rc = spi_rx(c, (uint8_t *)buf, len);
    cs_deassert(c);

    return rc;
}

/* ─── HAL write callback (page-program, 256B page aligned) ───── */

/**
 * NOR flash page program constraints:
 *   1. Write Enable must be sent before each PP command.
 *   2. A single PP command may not cross a 256B page boundary.
 *   3. WIP must be polled after each PP until complete.
 *
 * This function splits writes at 256B boundaries automatically.
 */
static int nor_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_nor_spi_ctx_t *c = (nvlog_nor_spi_ctx_t *)user;

    uint8_t  hdr[5];
    uint32_t offset    = 0;
    uint32_t remaining = len;

    while (remaining > 0) {
        uint32_t cur_addr = addr + offset;

        /* bytes available until end of current 256B page */
        uint32_t page_off = cur_addr % NOR_PAGE_SIZE;
        uint32_t space    = NOR_PAGE_SIZE - page_off;
        uint32_t n        = remaining < space ? remaining : space;

        /* WREN required before every PP */
        if (nor_write_enable(c) != 0)
            return -1;

        /* PP opcode + address */
        hdr[0] = NOR_OP_PP;
        uint8_t addr_len = encode_addr(cur_addr, c->glue.addr_bytes, hdr + 1);
        uint8_t hdr_len  = 1u + addr_len;

        cs_assert(c);
        int rc = spi_tx(c, hdr, hdr_len);
        if (rc == 0)
            rc = spi_tx(c, (const uint8_t *)buf + offset, n);
        cs_deassert(c);

        if (rc != 0)
            return -1;

        /* wait for internal write to complete */
        if (nor_wait_ready(c) != 0)
            return -1;

        offset    += n;
        remaining -= n;
    }

    return 0;
}

/* ─── HAL erase callback (4KB sector erase) ─────────────────── */

/**
 * Erase one or more 4KB sectors.
 * addr and len must both be NOR_SECTOR_SIZE (4096) aligned.
 */
static int nor_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_nor_spi_ctx_t *c = (nvlog_nor_spi_ctx_t *)user;

    if (addr % NOR_SECTOR_SIZE != 0) return -1;
    if (len  % NOR_SECTOR_SIZE != 0) return -1;

    uint8_t hdr[5];

    for (uint32_t off = 0; off < len; off += NOR_SECTOR_SIZE) {
        uint32_t cur_addr = addr + off;

        if (nor_write_enable(c) != 0)
            return -1;

        hdr[0] = NOR_OP_SE;
        uint8_t addr_len = encode_addr(cur_addr, c->glue.addr_bytes, hdr + 1);
        uint8_t hdr_len  = 1u + addr_len;

        cs_assert(c);
        int rc = spi_tx(c, hdr, hdr_len);
        cs_deassert(c);

        if (rc != 0)
            return -1;

        /* sector erase: up to 400ms on worst-case flash */
        if (nor_wait_ready(c) != 0)
            return -1;
    }

    return 0;
}

/* ─── init ───────────────────────────────────────────────────── */

nvlog_status_t nvlog_nor_spi_init(nvlog_nor_spi_ctx_t        *ctx,
                                   nvlog_hal_flash_t          *flash_out,
                                   const nvlog_nor_spi_glue_t *glue)
{
    if (!ctx || !flash_out || !glue)             return NVLOG_ERR_PARAM;
    if (!glue->cs_set || !glue->transfer)        return NVLOG_ERR_PARAM;
    if (glue->addr_bytes != 3 &&
        glue->addr_bytes != 4)                   return NVLOG_ERR_PARAM;
    if (glue->busy_timeout_ms == 0)              return NVLOG_ERR_PARAM;

    ctx->glue = *glue;

    flash_out->base.read  = nor_read;
    flash_out->base.write = nor_write;
    flash_out->base.user  = ctx;

    flash_out->erase      = nor_erase;
    flash_out->erase_size = NOR_SECTOR_SIZE;
    flash_out->prog_size  = NOR_PAGE_SIZE;
    flash_out->user       = ctx;

    return NVLOG_OK;
}

/* ─── JEDEC ID ───────────────────────────────────────────────── */

nvlog_status_t nvlog_nor_spi_read_jedec_id(nvlog_nor_spi_ctx_t *ctx,
                                            uint8_t *id_out)
{
    if (!ctx || !id_out) return NVLOG_ERR_PARAM;

    uint8_t cmd = NOR_OP_RDID;
    cs_assert(ctx);
    spi_tx(ctx, &cmd, 1);
    int rc = spi_rx(ctx, id_out, 3);
    cs_deassert(ctx);

    return rc == 0 ? NVLOG_OK : NVLOG_ERR_IO;
}
