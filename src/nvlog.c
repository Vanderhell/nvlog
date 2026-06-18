/**
 * nvlog.c — implementation
 *
 * Ring mode invariant: write_ptr is ALWAYS in [REGION_HEADER_SIZE, region_size).
 * After writing a record that fills exactly to region_size, write_ptr is
 * immediately normalized to REGION_HEADER_SIZE. This ensures the iterator
 * stop condition (cursor == stop_ptr) is always reachable.
 */

#include "nvlog.h"
#include <string.h>
#include "nvlog_wire.h"

#define NVLOG_SUPERBLOCK_PAYLOAD_SIZE 24u

#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;
    uint8_t  type;
    uint8_t  version;
    uint8_t  flags;
    uint32_t generation;
    uint32_t seq;
    uint16_t payload_len;
    uint16_t total_len;
    uint32_t crc32;
} nvlog_rec_hdr_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  type;
    uint8_t  version;
    uint8_t  flags;
    uint32_t generation;
    uint32_t seq;
    uint16_t payload_len;
    uint16_t total_len;
    uint32_t crc32;
} nvlog_rec_hdr_t;
#endif

typedef struct {
    uint8_t  magic;
    uint8_t  type;
    uint8_t  version;
    uint8_t  flags;
    uint32_t generation;
    uint32_t seq;
    uint16_t payload_len;
    uint16_t total_len;
    uint32_t crc32;
} nvlog_wire_rec_t;

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint8_t  mode;
    uint8_t  reserved0;
    uint32_t region_size;
    uint32_t generation;
    uint32_t metadata_seq;
    uint32_t feature_flags;
    uint32_t crc32;
} nvlog_superblock_t;

static int hal_read(nvlog_ctx_t *ctx, uint32_t addr, void *buf, uint32_t len);
static int hal_write(nvlog_ctx_t *ctx, uint32_t addr, const void *buf, uint32_t len);

static void sb_encode(uint8_t *dst, const nvlog_superblock_t *sb)
{
    nvlog_store_u32le(dst + 0, sb->magic);
    nvlog_store_u16le(dst + 4, sb->format_version);
    dst[6] = sb->mode;
    dst[7] = sb->reserved0;
    nvlog_store_u32le(dst + 8, sb->region_size);
    nvlog_store_u32le(dst + 12, sb->generation);
    nvlog_store_u32le(dst + 16, sb->metadata_seq);
    nvlog_store_u32le(dst + 20, sb->feature_flags);
    nvlog_store_u32le(dst + 24, sb->crc32);
}

static int sb_decode(nvlog_superblock_t *sb, const uint8_t *src)
{
    if (!sb || !src) return -1;
    sb->magic = nvlog_load_u32le(src + 0);
    sb->format_version = nvlog_load_u16le(src + 4);
    sb->mode = src[6];
    sb->reserved0 = src[7];
    sb->region_size = nvlog_load_u32le(src + 8);
    sb->generation = nvlog_load_u32le(src + 12);
    sb->metadata_seq = nvlog_load_u32le(src + 16);
    sb->feature_flags = nvlog_load_u32le(src + 20);
    sb->crc32 = nvlog_load_u32le(src + 24);
    return 0;
}

static uint32_t sb_crc(const uint8_t *src)
{
    return nvlog_crc32_bytes(src, NVLOG_SUPERBLOCK_PAYLOAD_SIZE);
}

static int sb_read(nvlog_ctx_t *ctx, uint32_t offset, nvlog_superblock_t *sb)
{
    uint8_t raw[NVLOG_SUPERBLOCK_SIZE];
    if (hal_read(ctx, offset, raw, sizeof(raw)) != 0) return -1;
    if (sb_decode(sb, raw) != 0) return -1;
    if (sb->magic != NVLOG_MEDIA_MAGIC) return -1;
    if (sb->format_version != NVLOG_MEDIA_VERSION) return -1;
    if (sb->reserved0 != 0) return -1;
    if (sb->feature_flags != 0) return -1;
    if (sb->mode != NVLOG_MODE_LINEAR && sb->mode != NVLOG_MODE_RING) return -1;
    if (sb->region_size != ctx->region_size) return -1;
    if (sb->crc32 != sb_crc(raw)) return -1;
    return 0;
}

static int sb_write(nvlog_ctx_t *ctx, uint32_t offset, const nvlog_superblock_t *sb)
{
    uint8_t raw[NVLOG_SUPERBLOCK_SIZE];
    nvlog_superblock_t temp = *sb;
    temp.crc32 = 0;
    sb_encode(raw, &temp);
    temp.crc32 = sb_crc(raw);
    sb_encode(raw, &temp);
    return hal_write(ctx, offset, raw, sizeof(raw));
}

static int generation_is_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static uint32_t next_generation_value(uint32_t current)
{
    current++;
    if (current == 0) current = 1;
    return current;
}

static uint32_t rec_header_crc(const uint8_t *raw)
{
    return nvlog_crc32_bytes(raw, 16u);
}

static void rec_encode(uint8_t *dst, const nvlog_wire_rec_t *rec)
{
    dst[0] = rec->magic;
    dst[1] = rec->type;
    dst[2] = rec->version;
    dst[3] = rec->flags;
    nvlog_store_u32le(dst + 4, rec->generation);
    nvlog_store_u32le(dst + 8, rec->seq);
    nvlog_store_u16le(dst + 12, rec->payload_len);
    nvlog_store_u16le(dst + 14, rec->total_len);
    nvlog_store_u32le(dst + 16, rec->crc32);
}

static int rec_decode(nvlog_wire_rec_t *rec, const uint8_t *src)
{
    if (!rec || !src) return -1;
    rec->magic = src[0];
    rec->type = src[1];
    rec->version = src[2];
    rec->flags = src[3];
    rec->generation = nvlog_load_u32le(src + 4);
    rec->seq = nvlog_load_u32le(src + 8);
    rec->payload_len = nvlog_load_u16le(src + 12);
    rec->total_len = nvlog_load_u16le(src + 14);
    rec->crc32 = nvlog_load_u32le(src + 16);
    return 0;
}

static int load_current_superblock(nvlog_ctx_t *ctx, nvlog_superblock_t *out)
{
    nvlog_superblock_t a, b;
    int have_a = sb_read(ctx, 0, &a) == 0;
    int have_b = sb_read(ctx, NVLOG_SUPERBLOCK_SIZE, &b) == 0;
    if (have_a && have_b) {
        *out = generation_is_newer(a.generation, b.generation) ? a : b;
        return 0;
    }
    if (have_a) { *out = a; return 0; }
    if (have_b) { *out = b; return 0; }
    return -1;
}

/* --- CRC32 ----------------------------------------------------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return crc;
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0u - (crc & 1u)));
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

static nvlog_status_t erase_region(nvlog_ctx_t *ctx)
{
    uint8_t fill[64];
    memset(fill, 0xFF, sizeof(fill));
    for (uint32_t off = 0; off < ctx->region_size; ) {
        uint32_t n = ctx->region_size - off;
        if (n > (uint32_t)sizeof(fill)) n = (uint32_t)sizeof(fill);
        if (hal_write(ctx, off, fill, n) != 0) return NVLOG_ERR_IO;
        off += n;
    }
    return NVLOG_OK;
}

/* --- verify_record -------------------------------------------- */

static int verify_record(nvlog_ctx_t *ctx, uint32_t cursor,
                          nvlog_rec_hdr_t *hdr_out, uint32_t *total_out)
{
    uint8_t raw[NVLOG_HEADER_SIZE];
    nvlog_wire_rec_t hdr;
    if (hal_read(ctx, cursor, raw, sizeof(raw)) != 0) return -1;
    if (rec_decode(&hdr, raw) != 0) return -1;

    if (hdr.magic != NVLOG_RECORD_MAGIC) return -1;
    if (hdr.version != NVLOG_RECORD_VERSION) return -1;
    if (hdr.type != NVLOG_RECORD_TYPE_DATA &&
        hdr.type != NVLOG_RECORD_TYPE_WRAP_PAD)
        return NVLOG_ERR_UNSUPPORTED;
    if (hdr.flags != 0u && hdr.flags != NVLOG_FLAGS_LINEAR &&
        hdr.flags != NVLOG_FLAGS_RING)
        return NVLOG_ERR_UNSUPPORTED;
    if (hdr.generation != ctx->generation) return NVLOG_ERR_STALE;
    if (hdr.payload_len > NVLOG_MAX_PAYLOAD) return NVLOG_ERR_BOUNDS;
    if (hdr.total_len != (uint16_t)(NVLOG_RECORD_OVERHEAD + hdr.payload_len))
        return NVLOG_ERR_BOUNDS;
    if (hdr.crc32 != rec_header_crc(raw)) return NVLOG_ERR_CORRUPT;

    uint32_t total = 0;
    uint32_t end = 0;
    uint32_t payload_off = 0;
    uint32_t crc_off = 0;
    if (!nvlog_u32_add_checked(NVLOG_RECORD_OVERHEAD, (uint32_t)hdr.payload_len, &total))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(cursor, total, &end)) return NVLOG_ERR_BOUNDS;
    if (end > ctx->region_size) return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(cursor, NVLOG_HEADER_SIZE, &payload_off)) return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(payload_off, hdr.payload_len, &crc_off)) return NVLOG_ERR_BOUNDS;

    uint32_t stored_crc = 0;
    if (hal_read(ctx, crc_off, &stored_crc, sizeof(stored_crc)) != 0) return NVLOG_ERR_IO;

    uint32_t crc = crc32_update(0, raw, sizeof(raw));
    uint8_t  chunk[16];
    uint32_t remaining = hdr.payload_len;
    uint32_t poff      = payload_off;
    while (remaining > 0) {
        uint32_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (hal_read(ctx, poff, chunk, n) != 0) return NVLOG_ERR_IO;
        crc       = crc32_update(crc, chunk, n);
        poff      += n;
        remaining -= n;
    }
    if (crc != stored_crc) return NVLOG_ERR_CORRUPT;

    if (hdr_out) {
        hdr_out->magic = hdr.magic;
        hdr_out->type = hdr.type;
        hdr_out->version = hdr.version;
        hdr_out->flags = hdr.flags;
        hdr_out->generation = hdr.generation;
        hdr_out->seq = hdr.seq;
        hdr_out->payload_len = hdr.payload_len;
        hdr_out->total_len = hdr.total_len;
        hdr_out->crc32 = hdr.crc32;
    }
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

    nvlog_ctx_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.hal = *hal;
    probe.region_size = region_size;
    nvlog_superblock_t current;
    uint32_t generation = 1;
    uint32_t metadata_seq = 0;
    if (load_current_superblock(&probe, &current) == 0) {
        generation = next_generation_value(current.generation);
        metadata_seq = current.metadata_seq + 1u;
        if (metadata_seq == 0) metadata_seq = 1u;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->hal         = *hal;
    ctx->region_size = region_size;
    ctx->write_ptr   = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->tail_ptr    = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->mode        = NVLOG_MODE_LINEAR;
    ctx->record_count = 0;
    ctx->generation  = generation;
    ctx->metadata_seq = metadata_seq;
    ctx->mutation    = 1;

    if (erase_region(ctx) != NVLOG_OK) return NVLOG_ERR_IO;
    nvlog_superblock_t sb = {
        .magic = NVLOG_MEDIA_MAGIC,
        .format_version = NVLOG_MEDIA_VERSION,
        .mode = NVLOG_MODE_LINEAR,
        .reserved0 = 0,
        .region_size = region_size,
        .generation = ctx->generation,
        .metadata_seq = ctx->metadata_seq,
        .feature_flags = 0,
        .crc32 = 0,
    };
    if (sb_write(ctx, 0, &sb) != 0) return NVLOG_ERR_IO;
    if (sb_write(ctx, NVLOG_SUPERBLOCK_SIZE, &sb) != 0) return NVLOG_ERR_IO;

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
    ctx->mutation    = 1;

    nvlog_superblock_t sb;
    if (load_current_superblock(ctx, &sb) != 0) return NVLOG_ERR_CORRUPT;
    ctx->mode = (nvlog_mode_t)sb.mode;
    ctx->generation = sb.generation;
    ctx->metadata_seq = sb.metadata_seq;
    ctx->mutation = 1;

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

    uint32_t total = 0;
    if (!nvlog_u32_add_checked(NVLOG_RECORD_OVERHEAD, (uint32_t)len, &total))
        return NVLOG_ERR_BOUNDS;

    uint32_t evicted = 0;

    if (ctx->mode == NVLOG_MODE_RING) {
        uint32_t capacity = ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        if (total > capacity) return NVLOG_ERR_FULL;

        /* wrap if record doesn't fit before region end */
        uint32_t write_end = 0;
        if (!nvlog_u32_add_checked(ctx->write_ptr, total, &write_end))
            return NVLOG_ERR_BOUNDS;
        if (write_end > ctx->region_size) {
            ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            ctx->ring_full = 1;
        }

        /* evict oldest records if we'd overwrite them */
        if (ctx->ring_full)
            evicted = ring_advance_tail(ctx, ctx->write_ptr, total);

    } else {
        uint32_t write_end = 0;
        if (!nvlog_u32_add_checked(ctx->write_ptr, total, &write_end))
            return NVLOG_ERR_BOUNDS;
        if (write_end > ctx->region_size)
            return NVLOG_ERR_FULL;
    }

    /* build record */
    nvlog_wire_rec_t hdr;
    hdr.magic = NVLOG_RECORD_MAGIC;
    hdr.type = NVLOG_RECORD_TYPE_DATA;
    hdr.version = NVLOG_RECORD_VERSION;
    hdr.flags = (ctx->mode == NVLOG_MODE_RING) ? NVLOG_FLAGS_RING : NVLOG_FLAGS_LINEAR;
    hdr.generation = ctx->generation;
    hdr.seq = ctx->next_seq;
    hdr.payload_len = len;
    hdr.total_len = (uint16_t)(NVLOG_RECORD_OVERHEAD + len);
    hdr.crc32 = 0;

    uint8_t hdr_raw[NVLOG_HEADER_SIZE];
    rec_encode(hdr_raw, &hdr);
    hdr.crc32 = rec_header_crc(hdr_raw);
    rec_encode(hdr_raw, &hdr);

    uint32_t crc = crc32_update(0, hdr_raw, sizeof(hdr_raw));
    if (len > 0)
        crc = crc32_update(crc, (const uint8_t *)payload, len);

    uint32_t base = ctx->write_ptr;
    uint32_t payload_off = 0;
    uint32_t crc_off = 0;
    if (!nvlog_u32_add_checked(base, NVLOG_HEADER_SIZE, &payload_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(payload_off, len, &crc_off))
        return NVLOG_ERR_BOUNDS;

    if (hal_write(ctx, base, hdr_raw, sizeof(hdr_raw)) != 0) return NVLOG_ERR_IO;
    if (len > 0)
        if (hal_write(ctx, payload_off, payload, len) != 0)
            return NVLOG_ERR_IO;
    if (hal_write(ctx, crc_off, &crc, sizeof(crc)) != 0)
        return NVLOG_ERR_IO;

    ctx->write_ptr += total;
    ctx->next_seq++;
    ctx->mutation++;

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
    it->snapshot_mutation = ctx->mutation;

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
    if (!it->ctx || !it->ctx->mounted) return NVLOG_ERR_NOT_MOUNTED;
    if (it->snapshot_mutation != it->ctx->mutation) return NVLOG_ERR_STALE;
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
        out->len    = hdr.payload_len;
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
                uint8_t raw[NVLOG_HEADER_SIZE];
                nvlog_wire_rec_t bad;
                if (hal_read(ctx, it->cursor, raw, sizeof(raw)) != 0)
                    return NVLOG_ERR_IO;
                if (raw[0] == 0xFFu)
                    return NVLOG_ERR_NO_DATA;
                if (rec_decode(&bad, raw) != 0 || bad.magic != NVLOG_RECORD_MAGIC)
                    return NVLOG_ERR_CORRUPT;
                if (bad.version != NVLOG_RECORD_VERSION ||
                    bad.type != NVLOG_RECORD_TYPE_DATA ||
                    bad.generation != ctx->generation)
                    return NVLOG_ERR_UNSUPPORTED;
                uint32_t bad_total = 0;
                if (!nvlog_u32_add_checked(NVLOG_RECORD_OVERHEAD,
                                           (uint32_t)bad.payload_len,
                                           &bad_total))
                    return NVLOG_ERR_BOUNDS;
                it->cursor += bad_total;
                continue;
            }

            uint32_t offset = it->cursor;
            it->cursor += total;

            out->seq    = hdr.seq;
            out->len    = hdr.payload_len;
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
    uint32_t header_off = 0;
    uint32_t payload_off = 0;
    if (!nvlog_u32_add_checked(record->offset, NVLOG_HEADER_SIZE, &header_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(header_off, record->len, &payload_off))
        return NVLOG_ERR_BOUNDS;
    if (payload_off > ctx->region_size) return NVLOG_ERR_BOUNDS;

    nvlog_rec_hdr_t hdr;
    uint32_t total = 0;
    if (verify_record(ctx, record->offset, &hdr, &total) != 0)
        return NVLOG_ERR_CORRUPT;
    if (hdr.seq != record->seq || hdr.payload_len != record->len)
        return NVLOG_ERR_STALE;

    if (hal_read(ctx, header_off, buf, record->len) != 0)
        return NVLOG_ERR_IO;
    return NVLOG_OK;
}

/* --- nvlog_stats ---------------------------------------------- */

nvlog_status_t nvlog_stats(nvlog_ctx_t *ctx, nvlog_stats_t *out)
{
    if (!ctx || !out)  return NVLOG_ERR_PARAM;
    if (!ctx->mounted) return NVLOG_ERR_NOT_MOUNTED;

    if (ctx->mode == NVLOG_MODE_RING) {
        uint32_t used = 0;
        uint32_t cursor = ctx->tail_ptr;
        uint32_t guard = 0;
        while (cursor != ctx->write_ptr &&
               cursor + NVLOG_RECORD_OVERHEAD <= ctx->region_size &&
               guard++ < ctx->record_count + 1u) {
            nvlog_rec_hdr_t hdr;
            uint32_t total;
            if (verify_record(ctx, cursor, &hdr, &total) != 0 || total == 0)
                break;
            used += total;
            cursor += total;
            if (cursor >= ctx->region_size)
                cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        }
        if (used == 0 && ctx->record_count > 0 && ctx->tail_ptr == ctx->write_ptr)
            used = ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        out->used_bytes = used;
        out->free_bytes = (used >= ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE)
                        ? 0
                        : (ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE - used);
    } else {
        out->used_bytes = ctx->write_ptr - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        out->free_bytes = ctx->region_size - ctx->write_ptr;
    }
    out->record_count = (ctx->mode == NVLOG_MODE_RING) ? ctx->record_count
                                                        : ctx->next_seq;
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

    nvlog_ctx_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.hal = *hal;
    probe.region_size = region_size;
    nvlog_superblock_t current;
    uint32_t generation = 1;
    uint32_t metadata_seq = 0;
    if (load_current_superblock(&probe, &current) == 0) {
        generation = next_generation_value(current.generation);
        metadata_seq = current.metadata_seq + 1u;
        if (metadata_seq == 0) metadata_seq = 1u;
    }

    nvlog_status_t st = nvlog_format(ctx, hal, region_size);
    if (st != NVLOG_OK) return st;

    ctx->mode         = NVLOG_MODE_RING;
    ctx->tail_ptr     = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->ring_full    = 0;
    ctx->record_count = 0;
    ctx->generation   = generation;
    ctx->metadata_seq = metadata_seq;
    ctx->mutation++;

    if (erase_region(ctx) != NVLOG_OK) return NVLOG_ERR_IO;
    nvlog_superblock_t sb = {
        .magic = NVLOG_MEDIA_MAGIC,
        .format_version = NVLOG_MEDIA_VERSION,
        .mode = NVLOG_MODE_RING,
        .reserved0 = 0,
        .region_size = region_size,
        .generation = ctx->generation,
        .metadata_seq = ctx->metadata_seq,
        .feature_flags = 0,
        .crc32 = 0,
    };
    if (sb_write(ctx, 0, &sb) != 0) return NVLOG_ERR_IO;
    if (sb_write(ctx, NVLOG_SUPERBLOCK_SIZE, &sb) != 0) return NVLOG_ERR_IO;
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

    nvlog_superblock_t sb;
    if (load_current_superblock(ctx, &sb) != 0) return NVLOG_ERR_CORRUPT;
    if (sb.mode != NVLOG_MODE_RING) return NVLOG_ERR_CORRUPT;
    ctx->generation = sb.generation;
    ctx->metadata_seq = sb.metadata_seq;

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
