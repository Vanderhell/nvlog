#include "nvlog_flash_sim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    nvlog_flash_sim_ctx_t *s = (nvlog_flash_sim_ctx_t *)user;
    const uint8_t *src = (const uint8_t *)buf;

    if (!s || (!src && len > 0)) return -1;
    if (addr > s->size || len > s->size - addr) return -1;
    if (s->program_size == 0 || s->program_alignment == 0) return -1;
    if (addr % s->program_alignment != 0) return -1;
    if (s->max_transfer != 0u && len > s->max_transfer) return -1;

    if (len == 0) return 0;
    if (len % s->program_size != 0) return -1;

    if (s->fail_after_write >= 0) {
        if (s->write_count >= (uint32_t)s->fail_after_write)
            return -1;
        s->write_count++;
    }

    if (s->fail_during_write >= 0 && (uint32_t)s->fail_during_write < len) {
        len = (uint32_t)s->fail_during_write;
    }

    for (uint32_t i = 0; i < len; i++) {
        uint8_t current = s->mem[addr + i];
        uint8_t next = src[i];
        if ((next & ~current) != 0) {
            s->bit_flip_violations++;
            fprintf(stderr,
                    "[nvlog_flash_sim] BIT FLIP VIOLATION at addr=0x%08X byte[%u]: current=0x%02X write=0x%02X\n",
                    addr, i, current, next);
            return -1;
        }
        s->mem[addr + i] = (uint8_t)(current & next);
    }

    if (s->fail_during_write >= 0 && (uint32_t)s->fail_during_write < len)
        return -1;

    return 0;
}

static int sim_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_flash_sim_ctx_t *s = (nvlog_flash_sim_ctx_t *)user;

    if (!s) return -1;
    if (addr % s->sector_size != 0) return -1;
    if (len % s->sector_size != 0) return -1;
    if (addr > s->size || len > s->size - addr) return -1;
    if (s->max_transfer != 0u && len > s->max_transfer) return -1;

    if (s->fail_after_erase >= 0) {
        if (s->erase_count_global >= (uint32_t)s->fail_after_erase)
            return -1;
        s->erase_count_global++;
    }

    if (s->fail_during_erase >= 0 && (uint32_t)s->fail_during_erase < len) {
        len = (uint32_t)s->fail_during_erase;
    }

    for (uint32_t off = 0; off < len; off += s->sector_size) {
        memset(s->mem + addr + off, s->erased_value, s->sector_size);
        uint32_t sec = (addr + off) / s->sector_size;
        if (sec < s->num_sectors)
            s->erase_counts[sec]++;
    }

    if (s->fail_during_erase >= 0 && (uint32_t)s->fail_during_erase < len)
        return -1;

    return 0;
}

static int flash_sim_open_cfg_inner(nvlog_flash_sim_ctx_t *sim,
                                    nvlog_hal_flash_t *flash_out,
                                    const nvlog_flash_sim_cfg_t *cfg)
{
    if (!sim || !flash_out || !cfg) return -1;
    if (cfg->capacity == 0 || cfg->sector_size == 0) return -1;
    if (cfg->program_unit == 0 || cfg->program_alignment == 0) return -1;
    if (cfg->erase_unit != 0u && cfg->erase_unit != cfg->sector_size) return -1;
    if (cfg->capacity % cfg->sector_size != 0) return -1;
    if (cfg->capacity / cfg->sector_size > 256u) return -1;
    if (cfg->sector_size % cfg->program_unit != 0) return -1;
    if (cfg->program_alignment % cfg->program_unit != 0) return -1;

    memset(sim, 0, sizeof(*sim));
    sim->mem = (uint8_t *)malloc(cfg->capacity);
    if (!sim->mem) return -1;

    sim->size = cfg->capacity;
    sim->sector_size = cfg->erase_unit != 0u ? cfg->erase_unit : cfg->sector_size;
    sim->program_size = cfg->program_unit;
    sim->program_alignment = cfg->program_alignment;
    sim->max_transfer = cfg->max_transfer;
    sim->erased_value = cfg->erased_value;
    sim->num_sectors = cfg->capacity / cfg->sector_size;
    sim->fail_after_write = -1;
    sim->fail_during_write = -1;
    sim->fail_after_erase = -1;
    sim->fail_during_erase = -1;

    memset(sim->mem, cfg->erased_value, cfg->capacity);

    flash_out->base.read = sim_read;
    flash_out->base.write = sim_write;
    flash_out->base.user = sim;
    flash_out->erase = sim_erase;
    flash_out->erase_size = sim->sector_size;
    flash_out->prog_size = cfg->program_unit;
    flash_out->user = sim;
    return 0;
}

int nvlog_flash_sim_open_cfg(nvlog_flash_sim_ctx_t *sim,
                             nvlog_hal_flash_t *flash_out,
                             const nvlog_flash_sim_cfg_t *cfg)
{
    return flash_sim_open_cfg_inner(sim, flash_out, cfg);
}

int nvlog_flash_sim_open(nvlog_flash_sim_ctx_t *sim,
                          nvlog_hal_flash_t *flash_out,
                          uint32_t size,
                          uint32_t sector_size)
{
    nvlog_flash_sim_cfg_t cfg;
    cfg.capacity = size;
    cfg.erased_value = 0xFFu;
    cfg.erase_unit = sector_size;
    cfg.program_unit = 1u;
    cfg.program_alignment = 1u;
    cfg.max_transfer = 0u;
    cfg.sector_size = sector_size;
    return flash_sim_open_cfg_inner(sim, flash_out, &cfg);
}

void nvlog_flash_sim_inject_write_fail(nvlog_flash_sim_ctx_t *sim, int32_t n)
{
    if (!sim) return;
    sim->fail_after_write = n;
    sim->write_count = 0;
}

void nvlog_flash_sim_inject_write_partial(nvlog_flash_sim_ctx_t *sim, int32_t bytes)
{
    if (!sim) return;
    sim->fail_during_write = bytes;
}

void nvlog_flash_sim_inject_erase_fail(nvlog_flash_sim_ctx_t *sim, int32_t n)
{
    if (!sim) return;
    sim->fail_after_erase = n;
    sim->erase_count_global = 0;
}

void nvlog_flash_sim_inject_erase_partial(nvlog_flash_sim_ctx_t *sim, int32_t bytes)
{
    if (!sim) return;
    sim->fail_during_erase = bytes;
}

void nvlog_flash_sim_reset(nvlog_flash_sim_ctx_t *sim)
{
    if (!sim || !sim->mem) return;
    memset(sim->mem, sim->erased_value, sim->size);
    memset(sim->erase_counts, 0, sizeof(sim->erase_counts));
    sim->bit_flip_violations = 0;
    sim->write_count = 0;
    sim->erase_count_global = 0;
    sim->fail_after_write = -1;
    sim->fail_during_write = -1;
    sim->fail_after_erase = -1;
    sim->fail_during_erase = -1;
}

void nvlog_flash_sim_close(nvlog_flash_sim_ctx_t *sim)
{
    if (!sim) return;
    free(sim->mem);
    sim->mem = NULL;
}
