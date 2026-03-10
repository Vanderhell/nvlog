/**
 * nvlog.c — implementation (v0.4, ring mode)
 *
 * Ring mode invariant: write_ptr is ALWAYS in [REGION_HEADER_SIZE, region_size).
 * After writing a record that fills exactly to region_size, write_ptr is
 * immediately normalized to REGION_HEADER_SIZE. This ensures the iterator
 * stop condition (cursor == stop_ptr) is always reachable.
 */

#include "nvlog.h"
#include <string.h>

/* --- internal record header ----------------------------------- */

#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;
    uint8_t  flags;
    uint16_t len;
    uint32_t seq;
} nvlog_rec_hdr_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  flags;
    uint16_t len;
    uint32_t seq;
} nvlog_rec_hdr_t;
#endif

/* --- CRC32 ----------------------------------------------------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return crc;
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return ~crc;
}

/* --- HAL wrappers --------------------------------------------- */

static int hal_read(nvlog_ctx_t *ctx, uint32_t addr, void *buf, uint32_t len)
{
    return ctx->hal.read(addr, buf, len, ctx->hal.user);
}

static int hal_write(nvlog_ctx_t *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    return ctx->hal.write(addr, buf, len, ctx->hal.user);
}

/* --- verify_record -------------------------------------------- */

static int verify_record(nvlog_ctx_t *ctx, uint32_t cursor,
                          nvlog_rec_hdr_t *hdr_out, uint32_t *total_out)
{
    nvlog_rec_hdr_t hdr;
    if (hal_read(ctx, cursor, &hdr, sizeof(hdr)) != 0) return -1;
    if (hdr.magic != NVLOG_RECORD_MAGIC)               return -1;

    uint32_t total = NVLOG_RECORD_OVERHEAD + hdr.len;
    if (cursor + total > ctx->region_size)             return -1;

    uint32_t stored_crc = 0;
    if (hal_read(ctx, cursor + NVLOG_HEADER_SIZE + hdr.len,
                 &stored_crc, sizeof(stored_crc)) != 0) return -1;

    uint32_t crc = crc32_update(0, (const uint8_t *)&hdr, sizeof(hdr));
    uint8_t  chunk[16];
    uint32_t remaining = hdr.len;
    uint32_t poff      = cursor + NVLOG_HEADER_SIZE;
    while (remaining > 0) {
        uint32_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (hal_read(ctx, poff, chunk, n) != 0) return -1;
        crc       = crc32_update(crc, chunk, n);
        poff      += n;
        remaining -= n;
    }
    if (crc != stored_crc) return -1;

    if (hdr_out)   *hdr_out   = hdr;
    if (total_out) *total_out = total;
    return 0;
}

/* --- write_ptr normalization ---------------------------------- */

/**
 * In ring mode, write_ptr must never equal region_size.
 * When a record ends exactly at region_size, wrap immediately.
 * This guarantees the iterator stop condition is reachable.
 */
static void normalize_write_ptr(nvlog_ctx_t *ctx)
{
    if (ctx->mode == NVLOG_MODE_RING &&
        ctx->write_ptr >= ctx->region_size) {
        ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        ctx->ring_full = 1;   /* wrapping = ring is full */
    }
}

/* --- nvlog_format --------------------------------------------- */

nvlog_status_t nvlog_format(nvlog_ctx_t *ctx,
                             const nvlog_hal_t *hal,
                             uint32_t region_size)
{
    if (!ctx || !hal || !hal->read || !hal->write)
        return NVLOG_ERR_PARAM;
    if (region_size <= NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_OVERHEAD)
        return NVLOG_ERR_PARAM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->hal         = *hal;
    ctx->region_size = region_size;
    ctx->write_ptr   = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->tail_ptr    = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->mode        = NVLOG_MODE_LINEAR;
    ctx->record_count = 0;

    nvlog_region_header_t rh;
    rh.magic       = NVLOG_REGION_MAGIC;
    rh.region_size = region_size;
    if (hal_write(ctx, 0, &rh, sizeof(rh)) != 0) return NVLOG_ERR_IO;

    ctx->mounted = 1;
    return NVLOG_OK;
}

/* --- nvlog_mount ---------------------------------------------- */

nvlog_status_t nvlog_mount(nvlog_ctx_t *ctx,
                            const nvlog_hal_t *hal,
                            uint32_t region_size)
{
    if (!ctx || !hal || !hal->read || !hal->write)
        return NVLOG_ERR_PARAM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->hal         = *hal;
    ctx->region_size = region_size;
    ctx->write_ptr   = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->tail_ptr    = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->mode        = NVLOG_MODE_LINEAR;

    nvlog_region_header_t rh;
    if (hal_read(ctx, 0, &rh, sizeof(rh)) != 0) return NVLOG_ERR_IO;
    if (rh.magic != NVLOG_REGION_MAGIC)          return NVLOG_ERR_CORRUPT;

    uint32_t cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    while (cursor + NVLOG_RECORD_OVERHEAD <= region_size) {
        nvlog_rec_hdr_t hdr;
        uint32_t        total;
        if (verify_record(ctx, cursor, &hdr, &total) != 0) break;
        ctx->write_ptr = cursor + total;
        ctx->next_seq  = hdr.seq + 1;
        cursor         = ctx->write_ptr;
    }

    ctx->mounted = 1;
    return NVLOG_OK;
}

/* --- ring: tail advancement ----------------------------------- */

/**
 * Advance tail_ptr past any record that overlaps [write_at, write_at+total).
 * Returns the number of records evicted.
 *
 * Overlap condition (only valid when ring_full=1):
 *   tail_ptr falls within the range the new record will overwrite.
 *
 * After normalization, write_ptr is always < region_size.
 * The new record always fits within [write_at, write_at+total) <= region_size
 * because nvlog_append already ensured the wrap.
 */
static uint32_t ring_advance_tail(nvlog_ctx_t *ctx,
                                  uint32_t write_at, uint32_t total)
{
    uint32_t write_end = write_at + total;
    uint32_t evicted = 0;

    while (ctx->tail_ptr >= write_at && ctx->tail_ptr < write_end) {
        nvlog_rec_hdr_t hdr;
        uint32_t        rec_total;

        if (verify_record(ctx, ctx->tail_ptr, &hdr, &rec_total) != 0)
            rec_total = (uint32_t)NVLOG_RECORD_OVERHEAD; /* skip minimum */

        ctx->tail_ptr += rec_total;

        if (ctx->tail_ptr >= ctx->region_size)
            ctx->tail_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;

        evicted++;

        /* safety: if tail wrapped past write_at, no more overlap possible */
        if (ctx->tail_ptr < write_at)
            break;
    }

    return evicted;
}

/* --- nvlog_append --------------------------------------------- */

nvlog_status_t nvlog_append(nvlog_ctx_t *ctx,
                             const void *payload,
                             uint16_t len)
{
    if (!ctx || (!payload && len > 0)) return NVLOG_ERR_PARAM;
    if (!ctx->mounted)                 return NVLOG_ERR_NOT_MOUNTED;
    (void)NVLOG_MAX_PAYLOAD;

    uint32_t total = NVLOG_RECORD_OVERHEAD + len;

    uint32_t evicted = 0;

    if (ctx->mode == NVLOG_MODE_RING) {
        uint32_t capacity = ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        if (total > capacity) return NVLOG_ERR_FULL;

        /* wrap if record doesn't fit before region end */
        if (ctx->write_ptr + total > ctx->region_size) {
            ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            ctx->ring_full = 1;
        }

        /* evict oldest records if we'd overwrite them */
        if (ctx->ring_full)
            evicted = ring_advance_tail(ctx, ctx->write_ptr, total);

    } else {
        if (ctx->write_ptr + total > ctx->region_size)
            return NVLOG_ERR_FULL;
    }

    /* build record */
    nvlog_rec_hdr_t hdr;
    hdr.magic = NVLOG_RECORD_MAGIC;
    hdr.flags = (ctx->mode == NVLOG_MODE_RING) ? NVLOG_FLAGS_RING : NVLOG_FLAGS_LINEAR;
    hdr.len   = len;
    hdr.seq   = ctx->next_seq;

    uint32_t crc = crc32_update(0, (const uint8_t *)&hdr, sizeof(hdr));
    if (len > 0)
        crc = crc32_update(crc, (const uint8_t *)payload, len);

    uint32_t base = ctx->write_ptr;

    if (hal_write(ctx, base, &hdr, sizeof(hdr)) != 0) return NVLOG_ERR_IO;
    if (len > 0)
        if (hal_write(ctx, base + NVLOG_HEADER_SIZE, payload, len) != 0)
            return NVLOG_ERR_IO;
    if (hal_write(ctx, base + NVLOG_HEADER_SIZE + len, &crc, sizeof(crc)) != 0)
        return NVLOG_ERR_IO;

    ctx->write_ptr += total;
    ctx->next_seq++;

    /* KEY: normalize write_ptr — must never equal region_size in ring mode */
    normalize_write_ptr(ctx);

    if (ctx->mode == NVLOG_MODE_RING) {
        ctx->record_count = (ctx->record_count > evicted)
                            ? ctx->record_count - evicted : 0;
        ctx->record_count++;
    }

    return NVLOG_OK;
}

/* --- nvlog_iter_init ------------------------------------------ */

nvlog_status_t nvlog_iter_init(nvlog_iter_t *it, nvlog_ctx_t *ctx)
{
    if (!it || !ctx)   return NVLOG_ERR_PARAM;
    if (!ctx->mounted) return NVLOG_ERR_NOT_MOUNTED;

    memset(it, 0, sizeof(*it));
    it->ctx = ctx;

    if (ctx->mode == NVLOG_MODE_RING) {
        it->cursor   = ctx->tail_ptr;
        it->stop_ptr = ctx->write_ptr;
    } else {
        it->cursor   = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        it->stop_ptr = ctx->region_size;
    }
    return NVLOG_OK;
}

/* --- nvlog_iter_next ------------------------------------------ */

nvlog_status_t nvlog_iter_next(nvlog_iter_t *it, nvlog_record_t *out)
{
    if (!it || !out) return NVLOG_ERR_PARAM;
    nvlog_ctx_t *ctx = it->ctx;

    if (ctx->mode == NVLOG_MODE_RING) {
        /*
         * Stop conditions (checked BEFORE reading the record at cursor):
         *
         *   1. Empty ring (ring_full=0, not yet wrapped):
         *      cursor starts at tail = write_ptr → nothing written yet.
         *
         *   2. Full lap completed (ring_full=1 or ring_full=0 partially filled):
         *      We've already yielded at least one record, and cursor has
         *      come back to stop_ptr (write_ptr). All records seen.
         *
         * This two-condition design avoids the ambiguity when
         * tail_ptr == write_ptr in a full ring (ring_full=1).
         */
        if (!ctx->ring_full && it->cursor == it->stop_ptr)
            return NVLOG_ERR_NO_DATA;  /* empty ring */

        if (it->count > 0 && it->cursor == it->stop_ptr)
            return NVLOG_ERR_NO_DATA;  /* exhausted */

        nvlog_rec_hdr_t hdr;
        uint32_t        total;

        if (verify_record(ctx, it->cursor, &hdr, &total) != 0)
            return NVLOG_ERR_NO_DATA;

        out->seq    = hdr.seq;
        out->len    = hdr.len;
        out->offset = it->cursor;
        it->count++;

        it->cursor += total;
        /* wrap cursor; normalized write_ptr ensures we'll hit stop_ptr */
        if (it->cursor >= ctx->region_size)
            it->cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;

        return NVLOG_OK;

    } else {
        /* linear mode — unchanged */
        while (it->cursor + NVLOG_RECORD_OVERHEAD <= ctx->region_size) {
            nvlog_rec_hdr_t hdr;
            uint32_t        total;
            int rc = verify_record(ctx, it->cursor, &hdr, &total);

            if (rc != 0) {
                uint8_t magic = 0;
                hal_read(ctx, it->cursor, &magic, 1);
                if (magic != NVLOG_RECORD_MAGIC)
                    return NVLOG_ERR_NO_DATA;
                /* corrupt record with valid magic: skip it */
                nvlog_rec_hdr_t bad;
                if (hal_read(ctx, it->cursor, &bad, sizeof(bad)) != 0)
                    return NVLOG_ERR_IO;
                it->cursor += NVLOG_RECORD_OVERHEAD + bad.len;
                continue;
            }

            uint32_t offset = it->cursor;
            it->cursor += total;

            out->seq    = hdr.seq;
            out->len    = hdr.len;
            out->offset = offset;
            it->count++;
            return NVLOG_OK;
        }
        return NVLOG_ERR_NO_DATA;
    }
}

/* --- nvlog_read_payload --------------------------------------- */

nvlog_status_t nvlog_read_payload(nvlog_ctx_t *ctx,
                                   const nvlog_record_t *record,
                                   void *buf, uint16_t buf_size)
{
    if (!ctx || !record || !buf) return NVLOG_ERR_PARAM;
    if (!ctx->mounted)           return NVLOG_ERR_NOT_MOUNTED;
    if (buf_size < record->len)  return NVLOG_ERR_PARAM;
    if (hal_read(ctx, record->offset + NVLOG_HEADER_SIZE, buf, record->len) != 0)
        return NVLOG_ERR_IO;
    return NVLOG_OK;
}

/* --- nvlog_stats ---------------------------------------------- */

nvlog_status_t nvlog_stats(nvlog_ctx_t *ctx, nvlog_stats_t *out)
{
    if (!ctx || !out)  return NVLOG_ERR_PARAM;
    if (!ctx->mounted) return NVLOG_ERR_NOT_MOUNTED;

    if (ctx->mode == NVLOG_MODE_RING && ctx->ring_full) {
        out->used_bytes = ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        out->free_bytes = 0;
    } else {
        out->used_bytes = ctx->write_ptr - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        out->free_bytes = ctx->region_size - ctx->write_ptr;
    }
    out->record_count = ctx->next_seq;
    out->next_seq     = ctx->next_seq;
    return NVLOG_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * RING MODE PUBLIC API
 * ═══════════════════════════════════════════════════════════════ */

nvlog_status_t nvlog_ring_format(nvlog_ctx_t *ctx,
                                  const nvlog_hal_t *hal,
                                  uint32_t region_size)
{
    if (region_size < (uint32_t)NVLOG_REGION_HEADER_SIZE + 2u * NVLOG_RECORD_OVERHEAD + 2u)
        return NVLOG_ERR_PARAM;

    nvlog_status_t st = nvlog_format(ctx, hal, region_size);
    if (st != NVLOG_OK) return st;

    ctx->mode         = NVLOG_MODE_RING;
    ctx->tail_ptr     = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->ring_full    = 0;
    ctx->record_count = 0;
    return NVLOG_OK;
}

/* --- nvlog_ring_mount ----------------------------------------- */

nvlog_status_t nvlog_ring_mount(nvlog_ctx_t *ctx,
                                 const nvlog_hal_t *hal,
                                 uint32_t region_size)
{
    if (!ctx || !hal || !hal->read || !hal->write) return NVLOG_ERR_PARAM;

    memset(ctx, 0, sizeof(*ctx));
    ctx->hal         = *hal;
    ctx->region_size = region_size;
    ctx->mode        = NVLOG_MODE_RING;

    nvlog_region_header_t rh;
    if (hal_read(ctx, 0, &rh, sizeof(rh)) != 0) return NVLOG_ERR_IO;
    if (rh.magic != NVLOG_REGION_MAGIC)          return NVLOG_ERR_CORRUPT;

    /* -- Pass 1: find record with highest SEQ ------------------- */
    uint32_t max_seq    = 0;
    uint32_t max_offset = 0;
    uint32_t max_total  = 0;
    uint8_t  found      = 0;
    uint32_t cursor     = (uint32_t)NVLOG_REGION_HEADER_SIZE;

    while (cursor + NVLOG_RECORD_OVERHEAD <= region_size) {
        nvlog_rec_hdr_t hdr;
        uint32_t        total;

        if (verify_record(ctx, cursor, &hdr, &total) == 0) {
            if (!found || hdr.seq > max_seq) {
                max_seq    = hdr.seq;
                max_offset = cursor;
                max_total  = total;
                found      = 1;
            }
            cursor += total;
        } else {
            cursor += NVLOG_RECORD_OVERHEAD;
        }
    }

    if (!found) {
        ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        ctx->tail_ptr  = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        ctx->next_seq  = 0;
        ctx->ring_full = 0;
        ctx->mounted   = 1;
        return NVLOG_OK;
    }

    ctx->next_seq  = max_seq + 1;

    /* derive write_ptr; normalize immediately */
    ctx->write_ptr = max_offset + max_total;
    if (ctx->write_ptr >= region_size)
        ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;

    /* -- Pass 2: find tail_ptr (oldest record) ------------------ */
    /*
     * Scan forward from write_ptr (wrapping), find first valid record.
     * That's the oldest record = tail.
     * Limit scan to (region_size - REGION_HEADER_SIZE) bytes to avoid loop.
     */
    uint32_t scan    = ctx->write_ptr;
    uint32_t limit   = region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
    uint32_t scanned = 0;

    ctx->tail_ptr  = ctx->write_ptr; /* default: empty */
    ctx->ring_full = 0;

    while (scanned < limit) {
        if (scan >= region_size)
            scan = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        if (scan == ctx->write_ptr && scanned > 0)
            break;  /* full circle, no tail */

        nvlog_rec_hdr_t hdr;
        uint32_t        total;

        if (verify_record(ctx, scan, &hdr, &total) == 0) {
            ctx->tail_ptr  = scan;
            ctx->ring_full = 1;
            break;
        }
        scan += NVLOG_RECORD_OVERHEAD;
        scanned++;
    }

    /* Count valid records in the ring by scanning from tail to write_ptr */
    ctx->record_count = 0;
    if (ctx->ring_full) {
        uint32_t cnt_scan = ctx->tail_ptr;
        uint32_t cnt_limit = region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        uint32_t cnt_scanned = 0;
        while (cnt_scanned < cnt_limit) {
            nvlog_rec_hdr_t hdr;
            uint32_t        total;
            if (verify_record(ctx, cnt_scan, &hdr, &total) == 0) {
                ctx->record_count++;
                cnt_scan += total;
            } else {
                cnt_scan += NVLOG_RECORD_OVERHEAD;
            }

            if (cnt_scan >= region_size)
                cnt_scan = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            if (cnt_scan == ctx->write_ptr)
                break;  /* reached write pointer */

            cnt_scanned++;
        }
    }

    ctx->mounted = 1;
    return NVLOG_OK;
}

/* --- nvlog_ring_count ----------------------------------------- */

uint32_t nvlog_ring_count(nvlog_ctx_t *ctx)
{
    if (!ctx || !ctx->mounted || ctx->mode != NVLOG_MODE_RING) return 0;
    return ctx->record_count;
}
