#include "nvlog_flash_sim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── callbacks ──────────────────────────────────────────────── */

static int sim_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_flash_sim_ctx_t *s = (nvlog_flash_sim_ctx_t *)user;
    if (!s || !buf) return -1;
    if (addr > s->size || len > s->size - addr) return -1;
    memcpy(buf, s->mem + addr, len);
    return 0;
}

static int sim_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_flash_sim_ctx_t *s   = (nvlog_flash_sim_ctx_t *)user;
    const uint8_t         *src = (const uint8_t *)buf;

    if (!s || (!src && len > 0)) return -1;
    if (addr > s->size || len > s->size - addr) return -1;

    /* power-loss injection */
    if (s->fail_after_write >= 0) {
        if (s->write_count >= (uint32_t)s->fail_after_write)
            return -1;
        s->write_count++;
    }

    for (uint32_t i = 0; i < len; i++) {
        uint8_t current = s->mem[addr + i];
        uint8_t next    = src[i];

        /* NOR physics: can only write 1→0, never 0→1 */
        if ((next & ~current) != 0) {
            /* attempted to set a bit that was already 0 */
            s->bit_flip_violations++;
            fprintf(stderr,
                "[nvlog_flash_sim] BIT FLIP VIOLATION at addr=0x%08X "
                "byte[%u]: current=0x%02X, write=0x%02X\n",
                addr, i, current, next);
            return -1;
        }
        s->mem[addr + i] = current & next;
    }
    return 0;
}

static int sim_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_flash_sim_ctx_t *s = (nvlog_flash_sim_ctx_t *)user;

    if (addr % s->sector_size != 0) return -1;
    if (len  % s->sector_size != 0) return -1;
    if (addr > s->size || len > s->size - addr) return -1;

    /* power-loss injection */
    if (s->fail_after_erase >= 0) {
        if (s->erase_count_global >= (uint32_t)s->fail_after_erase)
            return -1;
        s->erase_count_global++;
    }

    for (uint32_t off = 0; off < len; off += s->sector_size) {
        memset(s->mem + addr + off, 0xFF, s->sector_size);
        uint32_t sec = (addr + off) / s->sector_size;
        if (sec < s->num_sectors)
            s->erase_counts[sec]++;
    }
    return 0;
}

/* ─── open ───────────────────────────────────────────────────── */

int nvlog_flash_sim_open(nvlog_flash_sim_ctx_t *sim,
                          nvlog_hal_flash_t     *flash_out,
                          uint32_t               size,
                          uint32_t               sector_size)
{
    if (!sim || !flash_out || size == 0 || sector_size == 0) return -1;
    if (size % sector_size != 0) return -1;
    if (size / sector_size > 256u) return -1;

    memset(sim, 0, sizeof(*sim));
    sim->mem        = (uint8_t *)malloc(size);
    if (!sim->mem) return -1;

    sim->size             = size;
    sim->sector_size      = sector_size;
    sim->num_sectors      = size / sector_size;
    sim->fail_after_write = -1;
    sim->fail_after_erase = -1;

    memset(sim->mem, 0xFF, size);

    flash_out->base.read  = sim_read;
    flash_out->base.write = sim_write;
    flash_out->base.user  = sim;
    flash_out->erase      = sim_erase;
    flash_out->erase_size = sector_size;
    flash_out->prog_size  = 1;
    flash_out->user       = sim;

    return 0;
}

/* ─── injection / reset ──────────────────────────────────────── */

void nvlog_flash_sim_inject_write_fail(nvlog_flash_sim_ctx_t *sim, int32_t n)
{
    if (!sim) return;
    sim->fail_after_write = n;
    sim->write_count      = 0;
}

void nvlog_flash_sim_inject_erase_fail(nvlog_flash_sim_ctx_t *sim, int32_t n)
{
    if (!sim) return;
    sim->fail_after_erase  = n;
    sim->erase_count_global = 0;
}

void nvlog_flash_sim_reset(nvlog_flash_sim_ctx_t *sim)
{
    if (!sim || !sim->mem) return;
    memset(sim->mem, 0xFF, sim->size);
    memset(sim->erase_counts, 0, sizeof(sim->erase_counts));
    sim->bit_flip_violations  = 0;
    sim->write_count          = 0;
    sim->erase_count_global   = 0;
    sim->fail_after_write     = -1;
    sim->fail_after_erase     = -1;
}

void nvlog_flash_sim_close(nvlog_flash_sim_ctx_t *sim)
{
    if (!sim) return;
    free(sim->mem);
    sim->mem = NULL;
}
