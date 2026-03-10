#include "nvlog_posix.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ─── HAL callbacks ───────────────────────────────────────────── */

static int posix_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_posix_ctx_t *p = (nvlog_posix_ctx_t *)user;

    if (p->ram) {
        if (addr + len > p->size) return -1;
        memcpy(buf, p->ram + addr, len);
        return 0;
    }

    if (fseek(p->fp, (long)addr, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, len, p->fp) != len)        return -1;
    return 0;
}

static int posix_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_posix_ctx_t *p = (nvlog_posix_ctx_t *)user;

    /* power-loss injection */
    if (p->fail_after >= 0) {
        if (p->write_count >= (uint32_t)p->fail_after)
            return -1;   /* simulated power loss */
        p->write_count++;
    }

    if (p->ram) {
        if (addr + len > p->size) return -1;
        memcpy(p->ram + addr, buf, len);
        return 0;
    }

    if (fseek(p->fp, (long)addr, SEEK_SET) != 0) return -1;
    if (fwrite(buf, 1, len, p->fp) != len)        return -1;
    fflush(p->fp);
    return 0;
}

/* ─── open helpers ────────────────────────────────────────────── */

int nvlog_posix_open_file(nvlog_posix_ctx_t *pctx,
                           nvlog_hal_t *hal_out,
                           const char *path,
                           uint32_t size)
{
    if (!pctx || !hal_out || !path || size == 0) return -1;

    memset(pctx, 0, sizeof(*pctx));
    pctx->fail_after = -1;
    pctx->size       = size;

    /* open or create */
    FILE *fp = fopen(path, "r+b");
    if (!fp) {
        /* create and pre-fill with 0xFF (simulates erased NVM) */
        fp = fopen(path, "w+b");
        if (!fp) return -1;
        uint8_t fill = 0xFF;
        for (uint32_t i = 0; i < size; i++)
            fwrite(&fill, 1, 1, fp);
        fflush(fp);
        rewind(fp);
    }
    pctx->fp = fp;

    hal_out->read  = posix_read;
    hal_out->write = posix_write;
    hal_out->user  = pctx;
    return 0;
}

int nvlog_posix_open_ram(nvlog_posix_ctx_t *pctx,
                          nvlog_hal_t *hal_out,
                          uint32_t size)
{
    if (!pctx || !hal_out || size == 0) return -1;

    memset(pctx, 0, sizeof(*pctx));
    pctx->fail_after = -1;
    pctx->size       = size;
    pctx->ram        = (uint8_t *)calloc(1, size);
    if (!pctx->ram) return -1;

    /* fill with 0xFF to simulate unformatted NVM */
    memset(pctx->ram, 0xFF, size);

    hal_out->read  = posix_read;
    hal_out->write = posix_write;
    hal_out->user  = pctx;
    return 0;
}

/* ─── inject ──────────────────────────────────────────────────── */

void nvlog_posix_inject_fail_after(nvlog_posix_ctx_t *pctx, int32_t n)
{
    if (!pctx) return;
    pctx->fail_after  = n;
    pctx->write_count = 0;
}

/* ─── close ───────────────────────────────────────────────────── */

void nvlog_posix_close(nvlog_posix_ctx_t *pctx)
{
    if (!pctx) return;
    if (pctx->fp)  { fclose(pctx->fp);  pctx->fp  = NULL; }
    if (pctx->ram) { free(pctx->ram);   pctx->ram = NULL; }
}
