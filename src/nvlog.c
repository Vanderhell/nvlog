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

#define NVLOG_SUPERBLOCK_PAYLOAD_SIZE 60u
#define NVLOG_RING_RESERVE_BYTES NVLOG_RECORD_OVERHEAD

#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;
    uint8_t  type;
    uint8_t  version;
    uint8_t  flags;
    uint8_t  mode;
    uint8_t  reserved[3];
    uint32_t generation;
    uint32_t seq;
    uint32_t payload_len;
    uint32_t total_len;
    uint32_t alloc_len;
    uint32_t crc32;
} nvlog_wire_rec_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  type;
    uint8_t  version;
    uint8_t  flags;
    uint8_t  mode;
    uint8_t  reserved[3];
    uint32_t generation;
    uint32_t seq;
    uint32_t payload_len;
    uint32_t total_len;
    uint32_t alloc_len;
    uint32_t crc32;
} nvlog_wire_rec_t;
#endif

typedef nvlog_wire_rec_t nvlog_rec_hdr_t;

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint8_t  mode;
    uint8_t  media_class;
    uint32_t region_size;
    uint32_t generation;
    uint32_t metadata_seq;
    uint32_t write_ptr;
    uint32_t tail_ptr;
    uint32_t next_seq;
    uint32_t record_count;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t padding_bytes;
    uint32_t reserve_bytes;
    uint32_t feature_flags;
    uint32_t reserved0;
    uint32_t crc32;
} nvlog_superblock_t;

static int hal_read(nvlog_ctx_t *ctx, uint32_t addr, void *buf, uint32_t len);
static int hal_write(nvlog_ctx_t *ctx, uint32_t addr, const void *buf, uint32_t len);
static int ring_publish_superblocks(nvlog_ctx_t *ctx);
static int verify_record(nvlog_ctx_t *ctx, uint32_t cursor,
                         nvlog_wire_rec_t *hdr_out, uint32_t *total_out);

static void sb_encode(uint8_t *dst, const nvlog_superblock_t *sb)
{
    nvlog_store_u32le(dst + 0, sb->magic);
    nvlog_store_u16le(dst + 4, sb->format_version);
    dst[6] = sb->mode;
    dst[7] = sb->media_class;
    nvlog_store_u32le(dst + 8, sb->region_size);
    nvlog_store_u32le(dst + 12, sb->generation);
    nvlog_store_u32le(dst + 16, sb->metadata_seq);
    nvlog_store_u32le(dst + 20, sb->write_ptr);
    nvlog_store_u32le(dst + 24, sb->tail_ptr);
    nvlog_store_u32le(dst + 28, sb->next_seq);
    nvlog_store_u32le(dst + 32, sb->record_count);
    nvlog_store_u32le(dst + 36, sb->used_bytes);
    nvlog_store_u32le(dst + 40, sb->free_bytes);
    nvlog_store_u32le(dst + 44, sb->padding_bytes);
    nvlog_store_u32le(dst + 48, sb->reserve_bytes);
    nvlog_store_u32le(dst + 52, sb->feature_flags);
    nvlog_store_u32le(dst + 56, sb->reserved0);
    nvlog_store_u32le(dst + 60, sb->crc32);
}

static int sb_decode(nvlog_superblock_t *sb, const uint8_t *src)
{
    if (!sb || !src) return -1;
    sb->magic = nvlog_load_u32le(src + 0);
    sb->format_version = nvlog_load_u16le(src + 4);
    sb->mode = src[6];
    sb->media_class = src[7];
    sb->region_size = nvlog_load_u32le(src + 8);
    sb->generation = nvlog_load_u32le(src + 12);
    sb->metadata_seq = nvlog_load_u32le(src + 16);
    sb->write_ptr = nvlog_load_u32le(src + 20);
    sb->tail_ptr = nvlog_load_u32le(src + 24);
    sb->next_seq = nvlog_load_u32le(src + 28);
    sb->record_count = nvlog_load_u32le(src + 32);
    sb->used_bytes = nvlog_load_u32le(src + 36);
    sb->free_bytes = nvlog_load_u32le(src + 40);
    sb->padding_bytes = nvlog_load_u32le(src + 44);
    sb->reserve_bytes = nvlog_load_u32le(src + 48);
    sb->feature_flags = nvlog_load_u32le(src + 52);
    sb->reserved0 = nvlog_load_u32le(src + 56);
    sb->crc32 = nvlog_load_u32le(src + 60);
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
    if (sb->media_class != NVLOG_MEDIA_CLASS_BYTE_WRITABLE &&
        sb->media_class != NVLOG_MEDIA_CLASS_ERASE_BEFORE_WRITE)
        return -1;
    if (ctx->media_class != 0 && sb->media_class != ctx->media_class)
        return -1;
    if (sb->mode != NVLOG_MODE_LINEAR && sb->mode != NVLOG_MODE_RING) return -1;
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

static int u32_is_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static int superblock_is_newer(const nvlog_superblock_t *a,
                               const nvlog_superblock_t *b)
{
    if (a->generation != b->generation)
        return u32_is_newer(a->generation, b->generation);
    if (a->metadata_seq != b->metadata_seq)
        return u32_is_newer(a->metadata_seq, b->metadata_seq);
    return 0;
}

static uint32_t next_generation_value(uint32_t current)
{
    current++;
    if (current == 0) current = 1;
    return current;
}

static uint32_t rec_header_crc(const uint8_t *raw)
{
    return nvlog_crc32_bytes(raw, 28u);
}

static uint32_t rec_total_bytes(uint32_t payload_len)
{
    return NVLOG_RECORD_OVERHEAD + payload_len;
}

static uint32_t rec_alloc_bytes(uint32_t payload_len)
{
    return rec_total_bytes(payload_len);
}

static void rec_encode(uint8_t *dst, const nvlog_wire_rec_t *rec)
{
    dst[0] = rec->magic;
    dst[1] = rec->type;
    dst[2] = rec->version;
    dst[3] = rec->flags;
    dst[4] = rec->mode;
    dst[5] = rec->reserved[0];
    dst[6] = rec->reserved[1];
    dst[7] = rec->reserved[2];
    nvlog_store_u32le(dst + 8, rec->generation);
    nvlog_store_u32le(dst + 12, rec->seq);
    nvlog_store_u32le(dst + 16, rec->payload_len);
    nvlog_store_u32le(dst + 20, rec->total_len);
    nvlog_store_u32le(dst + 24, rec->alloc_len);
    nvlog_store_u32le(dst + 28, rec->crc32);
}

static int rec_decode(nvlog_wire_rec_t *rec, const uint8_t *src)
{
    if (!rec || !src) return -1;
    rec->magic = src[0];
    rec->type = src[1];
    rec->version = src[2];
    rec->flags = src[3];
    rec->mode = src[4];
    rec->reserved[0] = src[5];
    rec->reserved[1] = src[6];
    rec->reserved[2] = src[7];
    rec->generation = nvlog_load_u32le(src + 8);
    rec->seq = nvlog_load_u32le(src + 12);
    rec->payload_len = nvlog_load_u32le(src + 16);
    rec->total_len = nvlog_load_u32le(src + 20);
    rec->alloc_len = nvlog_load_u32le(src + 24);
    rec->crc32 = nvlog_load_u32le(src + 28);
    return 0;
}

static int load_current_superblock(nvlog_ctx_t *ctx, nvlog_superblock_t *out)
{
    nvlog_superblock_t a, b;
    int have_a = sb_read(ctx, 0, &a) == 0;
    int have_b = sb_read(ctx, NVLOG_SUPERBLOCK_SIZE, &b) == 0;
    if (have_a && have_b) {
        *out = superblock_is_newer(&a, &b) ? a : b;
        return 0;
    }
    if (have_a) { *out = a; return 0; }
    if (have_b) { *out = b; return 0; }
    return -1;
}

static uint32_t ring_capacity_bytes(const nvlog_ctx_t *ctx)
{
    if (!ctx || ctx->region_size <= (uint32_t)NVLOG_REGION_HEADER_SIZE)
        return 0;
    return ctx->region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
}

static uint32_t ring_usable_bytes(const nvlog_ctx_t *ctx)
{
    uint32_t capacity = ring_capacity_bytes(ctx);
    uint32_t reserve = (ctx && ctx->record_count > 0) ? ctx->reserve_bytes : 0u;
    if (capacity <= reserve) return 0;
    return capacity - reserve;
}

nvlog_status_t format_impl(nvlog_ctx_t *ctx,
                           const nvlog_hal_t *hal,
                           uint32_t region_size,
                           nvlog_mode_t mode,
                           uint8_t media_class,
                           uint8_t program_unit,
                           uint8_t erased_value,
                           uint32_t geometry_key)
{
    nvlog_ctx_t probe;
    nvlog_superblock_t current;
    uint32_t generation = 1;
    uint32_t metadata_seq = 0;

    memset(&probe, 0, sizeof(probe));
    probe.hal = *hal;
    probe.region_size = region_size;
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
    ctx->mode        = mode;
    ctx->generation  = generation;
    ctx->metadata_seq = metadata_seq;
    ctx->mutation    = 1;
    ctx->geometry_key = geometry_key;
    ctx->media_class = media_class;
    ctx->program_unit = program_unit;
    ctx->erased_value = erased_value;
    ctx->record_count = 0;
    ctx->used_bytes = 0;
    ctx->padding_bytes = 0;
    ctx->reserve_bytes = (mode == NVLOG_MODE_RING) ? NVLOG_RING_RESERVE_BYTES : 0;
    ctx->free_bytes = (mode == NVLOG_MODE_RING)
                    ? ring_usable_bytes(ctx)
                    : (region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE);

    nvlog_superblock_t sb = {
        .magic = NVLOG_MEDIA_MAGIC,
        .format_version = NVLOG_MEDIA_VERSION,
        .mode = mode,
        .media_class = media_class,
        .region_size = region_size,
        .generation = generation,
        .metadata_seq = metadata_seq,
        .write_ptr = ctx->write_ptr,
        .tail_ptr = ctx->tail_ptr,
        .next_seq = 0,
        .record_count = 0,
        .used_bytes = 0,
        .free_bytes = ctx->free_bytes,
        .padding_bytes = 0,
        .reserve_bytes = ctx->reserve_bytes,
        .feature_flags = 0,
        .reserved0 = 0,
        .crc32 = 0,
    };

    if (mode == NVLOG_MODE_RING) {
        if (ring_publish_superblocks(ctx) != 0) return NVLOG_ERR_IO;
    } else {
        if (sb_write(ctx, 0, &sb) != 0) return NVLOG_ERR_IO;
        if (sb_write(ctx, NVLOG_SUPERBLOCK_SIZE, &sb) != 0) return NVLOG_ERR_IO;
    }
    ctx->mounted = 1;
    return NVLOG_OK;
}

static int ring_publish_superblocks(nvlog_ctx_t *ctx)
{
    nvlog_superblock_t sb = {
        .magic = NVLOG_MEDIA_MAGIC,
        .format_version = NVLOG_MEDIA_VERSION,
        .mode = NVLOG_MODE_RING,
        .media_class = ctx->media_class,
        .region_size = ctx->region_size,
        .generation = ctx->generation,
        .metadata_seq = ctx->metadata_seq,
        .write_ptr = ctx->write_ptr,
        .tail_ptr = ctx->tail_ptr,
        .next_seq = ctx->next_seq,
        .record_count = ctx->record_count,
        .used_bytes = ctx->used_bytes,
        .free_bytes = ctx->free_bytes,
        .padding_bytes = ctx->padding_bytes,
        .reserve_bytes = ctx->reserve_bytes,
        .feature_flags = 0,
        .reserved0 = 0,
        .crc32 = 0,
    };
    if (sb_write(ctx, 0, &sb) != 0) return -1;
    if (sb_write(ctx, NVLOG_SUPERBLOCK_SIZE, &sb) != 0) return -1;
    return 0;
}

static uint32_t ring_count_records(const nvlog_ctx_t *ctx)
{
    return ctx ? ctx->record_count : 0;
}

static int ring_validate_window(nvlog_ctx_t *ctx)
{
    uint32_t occupied = 0;
    uint32_t cursor = 0;
    uint32_t data_count = 0;
    uint32_t data_bytes = 0;
    uint32_t padding_bytes = 0;
    uint32_t last_seq = 0;
    int have_seq = 0;

    if (!ctx) return -1;
    if (ctx->tail_ptr >= ctx->region_size || ctx->write_ptr >= ctx->region_size)
        return -1;
    if (ctx->write_ptr == ctx->tail_ptr) {
        occupied = (ctx->record_count > 0) ? ring_capacity_bytes(ctx) : 0u;
    } else if (ctx->write_ptr > ctx->tail_ptr) {
        occupied = ctx->write_ptr - ctx->tail_ptr;
    } else {
        uint32_t tail_gap = ctx->region_size - ctx->tail_ptr;
        uint32_t head_span = ctx->write_ptr - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        if (!nvlog_u32_add_checked(tail_gap, head_span, &occupied))
            return -1;
    }
    cursor = ctx->tail_ptr;
    while (occupied > 0) {
        if (cursor != (uint32_t)NVLOG_REGION_HEADER_SIZE &&
            cursor + NVLOG_RECORD_OVERHEAD > ctx->region_size &&
            ctx->write_ptr < cursor) {
            uint32_t gap = ctx->region_size - cursor;
            if (gap > occupied) return -1;
            padding_bytes += gap;
            occupied -= gap;
            cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            continue;
        }
        nvlog_wire_rec_t hdr;
        uint32_t rec_total = 0;
        int rc = verify_record(ctx, cursor, &hdr, &rec_total);
        if (rc != 0) return rc;
        if (rec_total == 0 || rec_total > occupied) return -1;
        if (hdr.type == NVLOG_RECORD_TYPE_DATA) {
            data_count++;
            data_bytes += rec_total;
            if (!have_seq) {
                last_seq = hdr.seq;
                have_seq = 1;
            } else {
                if ((uint32_t)(last_seq + 1u) != hdr.seq) return -1;
                last_seq = hdr.seq;
            }
        } else if (hdr.type == NVLOG_RECORD_TYPE_WRAP ||
                   hdr.type == NVLOG_RECORD_TYPE_PADDING) {
            padding_bytes += rec_total;
        } else {
            return NVLOG_ERR_UNSUPPORTED;
        }
        occupied -= rec_total;
        cursor += rec_total;
        if (cursor >= ctx->region_size)
            cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
    }

    if (cursor != ctx->write_ptr) return -1;
    ctx->record_count = data_count;
    ctx->used_bytes = data_bytes;
    ctx->padding_bytes = padding_bytes;
    if (have_seq)
        ctx->next_seq = (uint32_t)(last_seq + 1u);
    else
        ctx->next_seq = 0u;
    {
        uint32_t usable = ring_capacity_bytes(ctx);
        uint32_t occupied_all = 0;
        if (!nvlog_u32_add_checked(data_bytes, padding_bytes, &occupied_all))
            return -1;
        ctx->free_bytes = (usable > occupied_all) ? (usable - occupied_all) : 0u;
        ctx->ring_full = (ctx->free_bytes == 0);
    }
    return 0;
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

static int write_record_wire(nvlog_ctx_t *ctx,
                             uint32_t base,
                             uint8_t type,
                             uint8_t flags,
                             uint8_t mode,
                             uint32_t seq,
                             uint32_t generation,
                             const void *payload,
                             uint32_t payload_len,
                             uint32_t *total_out)
{
    uint32_t record_total = rec_total_bytes(payload_len);
    uint32_t payload_off = 0;
    uint32_t crc_off = 0;
    uint32_t commit_off = 0;
    uint32_t end_off = 0;
    uint8_t raw[NVLOG_RECORD_HEADER_SIZE];
    nvlog_wire_rec_t rec = {
        .magic = NVLOG_RECORD_MAGIC,
        .type = type,
        .version = NVLOG_RECORD_VERSION,
        .flags = flags,
        .mode = mode,
        .reserved = {0, 0, 0},
        .generation = generation,
        .seq = seq,
        .payload_len = payload_len,
        .total_len = record_total,
        .alloc_len = record_total,
        .crc32 = 0,
    };

    if (!nvlog_u32_add_checked(base, NVLOG_RECORD_HEADER_SIZE, &payload_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(payload_off, payload_len, &crc_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(crc_off, NVLOG_RECORD_CRC_SIZE, &commit_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(base, record_total, &end_off))
        return NVLOG_ERR_BOUNDS;
    if (end_off > ctx->region_size) return NVLOG_ERR_BOUNDS;

    rec_encode(raw, &rec);
    rec.crc32 = rec_header_crc(raw);
    rec_encode(raw, &rec);

    uint32_t payload_crc = crc32_update(0, raw, sizeof(raw));
    if (payload_len > 0) {
        uint8_t zero_chunk[16] = {0};
        const uint8_t *payload_ptr = payload ? (const uint8_t *)payload : zero_chunk;
        uint32_t remaining = payload_len;
        while (remaining > 0) {
            uint32_t n = remaining < sizeof(zero_chunk) ? remaining : sizeof(zero_chunk);
            payload_crc = crc32_update(payload_crc, payload_ptr, n);
            if (!payload)
                payload_ptr = zero_chunk;
            else
                payload_ptr += n;
            remaining -= n;
        }
    }

    uint8_t commit = 0xFFu;
    if (hal_write(ctx, commit_off, &commit, sizeof(commit)) != 0) return NVLOG_ERR_IO;
    if (hal_write(ctx, base, raw, sizeof(raw)) != 0) return NVLOG_ERR_IO;
    if (payload_len > 0) {
        if (payload) {
            if (hal_write(ctx, payload_off, payload, payload_len) != 0) return NVLOG_ERR_IO;
        } else {
            uint8_t zero_chunk[16] = {0};
            uint32_t remaining = payload_len;
            uint32_t off = payload_off;
            while (remaining > 0) {
                uint32_t n = remaining < sizeof(zero_chunk) ? remaining : sizeof(zero_chunk);
                if (hal_write(ctx, off, zero_chunk, n) != 0) return NVLOG_ERR_IO;
                off += n;
                remaining -= n;
            }
        }
    }
    if (hal_write(ctx, crc_off, &payload_crc, sizeof(payload_crc)) != 0) return NVLOG_ERR_IO;
    commit = 0x00u;
    if (hal_write(ctx, commit_off, &commit, sizeof(commit)) != 0) return NVLOG_ERR_IO;

    if (total_out) *total_out = record_total;
    return NVLOG_OK;
}

/* --- verify_record -------------------------------------------- */

static int verify_record(nvlog_ctx_t *ctx, uint32_t cursor,
                         nvlog_wire_rec_t *hdr_out, uint32_t *total_out)
{
    uint8_t raw[NVLOG_RECORD_HEADER_SIZE];
    nvlog_wire_rec_t hdr;
    if (hal_read(ctx, cursor, raw, sizeof(raw)) != 0) return -1;
    if (rec_decode(&hdr, raw) != 0) return -1;

    if (hdr.magic != NVLOG_RECORD_MAGIC) return -1;
    if (hdr.version != NVLOG_RECORD_VERSION) return -1;
    if (hdr.mode != NVLOG_MODE_LINEAR && hdr.mode != NVLOG_MODE_RING)
        return NVLOG_ERR_UNSUPPORTED;
    if (hdr.flags != NVLOG_FLAGS_LINEAR && hdr.flags != NVLOG_FLAGS_RING)
        return NVLOG_ERR_UNSUPPORTED;
    if (hdr.reserved[0] != 0u || hdr.reserved[1] != 0u || hdr.reserved[2] != 0u)
        return NVLOG_ERR_UNSUPPORTED;
    if (hdr.type != NVLOG_RECORD_TYPE_DATA &&
        hdr.type != NVLOG_RECORD_TYPE_WRAP &&
        hdr.type != NVLOG_RECORD_TYPE_PADDING)
        return NVLOG_ERR_UNSUPPORTED;
    if (hdr.generation != ctx->generation) return NVLOG_ERR_STALE;
    if (hdr.type == NVLOG_RECORD_TYPE_DATA && hdr.payload_len > NVLOG_MAX_PAYLOAD)
        return NVLOG_ERR_TOO_LARGE;
    if (hdr.total_len != rec_total_bytes(hdr.payload_len))
        return NVLOG_ERR_BOUNDS;
    if (hdr.alloc_len != rec_alloc_bytes(hdr.payload_len))
        return NVLOG_ERR_BOUNDS;
    if (hdr.crc32 != rec_header_crc(raw)) return NVLOG_ERR_CORRUPT;

    uint32_t total = 0;
    uint32_t end = 0;
    uint32_t payload_off = 0;
    uint32_t crc_off = 0;
    if (!nvlog_u32_add_checked(NVLOG_RECORD_OVERHEAD, hdr.payload_len, &total))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(cursor, total, &end)) return NVLOG_ERR_BOUNDS;
    if (end > ctx->region_size) return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(cursor, NVLOG_RECORD_HEADER_SIZE, &payload_off)) return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(payload_off, hdr.payload_len, &crc_off)) return NVLOG_ERR_BOUNDS;

    uint32_t stored_crc = 0;
    uint8_t commit = 0x00u;
    if (hal_read(ctx, crc_off, &stored_crc, sizeof(stored_crc)) != 0) return NVLOG_ERR_IO;
    if (hal_read(ctx, crc_off + sizeof(stored_crc), &commit, sizeof(commit)) != 0) return NVLOG_ERR_IO;

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
    if (commit != 0x00u) return NVLOG_ERR_NO_DATA;

    if (hdr_out) {
        hdr_out->magic = hdr.magic;
        hdr_out->type = hdr.type;
        hdr_out->version = hdr.version;
        hdr_out->flags = hdr.flags;
        hdr_out->mode = hdr.mode;
        hdr_out->reserved[0] = hdr.reserved[0];
        hdr_out->reserved[1] = hdr.reserved[1];
        hdr_out->reserved[2] = hdr.reserved[2];
        hdr_out->generation = hdr.generation;
        hdr_out->seq = hdr.seq;
        hdr_out->payload_len = hdr.payload_len;
        hdr_out->total_len = hdr.total_len;
        hdr_out->alloc_len = hdr.alloc_len;
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
    return format_impl(ctx, hal, region_size, NVLOG_MODE_LINEAR,
                       NVLOG_MEDIA_CLASS_BYTE_WRITABLE, 1u, 0xFFu, 0u);
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
    ctx->used_bytes  = 0;
    ctx->free_bytes  = region_size - (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->padding_bytes = 0;
    ctx->reserve_bytes = 0;
    ctx->mutation    = 1;
    ctx->geometry_key = 0;
    ctx->media_class = 0;
    ctx->program_unit = 1u;
    ctx->erased_value = 0xFFu;

    nvlog_superblock_t sb;
    if (load_current_superblock(ctx, &sb) != 0) return NVLOG_ERR_CORRUPT;
    if (sb.mode != NVLOG_MODE_LINEAR) return NVLOG_ERR_UNSUPPORTED;
    if (sb.region_size != region_size) return NVLOG_ERR_CORRUPT;
    ctx->mode = (nvlog_mode_t)sb.mode;
    ctx->media_class = sb.media_class;
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

    ctx->used_bytes = ctx->write_ptr - (uint32_t)NVLOG_REGION_HEADER_SIZE;
    ctx->free_bytes = ctx->region_size - ctx->write_ptr;
    ctx->record_count = ctx->next_seq;

    ctx->mounted = 1;
    return NVLOG_OK;
}

/* --- nvlog_append --------------------------------------------- */

nvlog_status_t nvlog_append(nvlog_ctx_t *ctx,
                              const void *payload,
                              size_t len)
{
    if (!ctx || (!payload && len > 0)) return NVLOG_ERR_PARAM;
    if (!ctx->mounted)                 return NVLOG_ERR_NOT_MOUNTED;
    if (len > UINT32_MAX)              return NVLOG_ERR_TOO_LARGE;
    if (len > NVLOG_MAX_PAYLOAD)       return NVLOG_ERR_TOO_LARGE;

    uint32_t payload_len = (uint32_t)len;
    uint32_t total = rec_total_bytes(payload_len);
    uint32_t evicted = 0;

    if (ctx->mode == NVLOG_MODE_RING) {
        uint32_t capacity = ring_capacity_bytes(ctx);
        uint32_t usable = ring_usable_bytes(ctx);
        uint32_t base = ctx->write_ptr;
        uint32_t new_tail = ctx->tail_ptr;
        uint32_t retired = 0;
        uint32_t occupied_live = 0;

        if (capacity == 0 || usable == 0) return NVLOG_ERR_FULL;
        if (total > capacity) return NVLOG_ERR_FULL;
        if (!nvlog_u32_add_checked(ctx->used_bytes, ctx->padding_bytes, &occupied_live))
            return NVLOG_ERR_BOUNDS;

        if (occupied_live + total > usable) {
            uint32_t retire_bytes = (occupied_live + total) - usable;
            uint32_t cursor = ctx->tail_ptr;
            while (retired < retire_bytes && evicted <= ctx->record_count) {
                if (cursor != (uint32_t)NVLOG_REGION_HEADER_SIZE &&
                    cursor + NVLOG_RECORD_OVERHEAD > ctx->region_size &&
                    ctx->write_ptr < cursor) {
                    uint32_t gap = ctx->region_size - cursor;
                    retired += gap;
                    cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
                    continue;
                }
                nvlog_wire_rec_t hdr;
                uint32_t rec_total = 0;
                int rc = verify_record(ctx, cursor, &hdr, &rec_total);
                if (rc != 0) return (nvlog_status_t)rc;
                retired += rec_total;
                if (hdr.type == NVLOG_RECORD_TYPE_DATA) {
                    evicted++;
                }
                cursor += rec_total;
                if (cursor >= ctx->region_size)
                    cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            }
            new_tail = cursor;
        }

        if (base + total > ctx->region_size) {
            uint32_t remaining = ctx->region_size - base;
            if (remaining > 0 && remaining < NVLOG_RECORD_OVERHEAD) {
                ctx->padding_bytes += remaining;
                ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;
                base = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            } else if (remaining > NVLOG_RECORD_OVERHEAD) {
                uint32_t pad_total = 0;
                int rc = write_record_wire(ctx, base,
                                           NVLOG_RECORD_TYPE_PADDING,
                                           NVLOG_FLAGS_RING,
                                           NVLOG_MODE_RING,
                                           ctx->next_seq,
                                           ctx->generation,
                                           NULL,
                                           remaining - NVLOG_RECORD_OVERHEAD,
                                           &pad_total);
                if (rc != NVLOG_OK) return (nvlog_status_t)rc;
                ctx->padding_bytes += pad_total;
            }
            base = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        }

        if (base + total > ctx->region_size)
            return NVLOG_ERR_FULL;

        if (base != ctx->write_ptr) {
            uint32_t wrap_total = 0;
            int rc = write_record_wire(ctx, ctx->write_ptr,
                                       NVLOG_RECORD_TYPE_WRAP,
                                       NVLOG_FLAGS_RING,
                                       NVLOG_MODE_RING,
                                       ctx->next_seq,
                                       ctx->generation,
                                       NULL,
                                       0,
                                       &wrap_total);
            if (rc != NVLOG_OK) return (nvlog_status_t)rc;
            ctx->padding_bytes += wrap_total;
        }

        {
            uint32_t written = 0;
            int rc = write_record_wire(ctx, base,
                                       NVLOG_RECORD_TYPE_DATA,
                                       NVLOG_FLAGS_RING,
                                       NVLOG_MODE_RING,
                                       ctx->next_seq,
                                       ctx->generation,
                                        payload,
                                        payload_len,
                                        &written);
            if (rc != NVLOG_OK) return (nvlog_status_t)rc;
            ctx->write_ptr = base + written;
            if (ctx->write_ptr >= ctx->region_size)
                ctx->write_ptr = (uint32_t)NVLOG_REGION_HEADER_SIZE;
        }

        ctx->tail_ptr = new_tail;
        ctx->record_count = (ctx->record_count > evicted ? ctx->record_count - evicted : 0u) + 1u;
        ctx->metadata_seq++;
        if (ctx->metadata_seq == 0) ctx->metadata_seq = 1u;
        ctx->mutation++;
        if (ring_validate_window(ctx) != 0) return NVLOG_ERR_CORRUPT;
        if (ring_publish_superblocks(ctx) != 0) return NVLOG_ERR_IO;
        return NVLOG_OK;
    } else {
        uint32_t write_end = 0;
        if (!nvlog_u32_add_checked(ctx->write_ptr, total, &write_end))
            return NVLOG_ERR_BOUNDS;
        if (write_end > ctx->region_size)
            return NVLOG_ERR_FULL;
    }

    if (ctx->mode != NVLOG_MODE_RING) {
        uint32_t written = 0;
        int rc = write_record_wire(ctx, ctx->write_ptr,
                                   NVLOG_RECORD_TYPE_DATA,
                                   NVLOG_FLAGS_LINEAR,
                                   NVLOG_MODE_LINEAR,
                                   ctx->next_seq,
                                   ctx->generation,
                                    payload,
                                    payload_len,
                                    &written);
        if (rc != NVLOG_OK) return (nvlog_status_t)rc;
        ctx->write_ptr += written;
        ctx->next_seq++;
        ctx->mutation++;
        ctx->used_bytes = ctx->write_ptr - (uint32_t)NVLOG_REGION_HEADER_SIZE;
        ctx->free_bytes = ctx->region_size - ctx->write_ptr;
        ctx->record_count = ctx->next_seq;
        return NVLOG_OK;
    }

    /* build record */
    nvlog_wire_rec_t hdr;
    hdr.magic = NVLOG_RECORD_MAGIC;
    hdr.type = NVLOG_RECORD_TYPE_DATA;
    hdr.version = NVLOG_RECORD_VERSION;
    hdr.flags = (ctx->mode == NVLOG_MODE_RING) ? NVLOG_FLAGS_RING : NVLOG_FLAGS_LINEAR;
    hdr.generation = ctx->generation;
    hdr.seq = ctx->next_seq;
    hdr.payload_len = payload_len;
    hdr.total_len = (uint32_t)(NVLOG_RECORD_OVERHEAD + payload_len);
    hdr.crc32 = 0;

    uint8_t hdr_raw[NVLOG_HEADER_SIZE];
    rec_encode(hdr_raw, &hdr);
    hdr.crc32 = rec_header_crc(hdr_raw);
    rec_encode(hdr_raw, &hdr);

    uint32_t crc = crc32_update(0, hdr_raw, sizeof(hdr_raw));
    if (payload_len > 0)
        crc = crc32_update(crc, (const uint8_t *)payload, payload_len);

    uint32_t base = ctx->write_ptr;
    uint32_t payload_off = 0;
    uint32_t crc_off = 0;
    uint32_t commit_off = 0;
    if (!nvlog_u32_add_checked(base, NVLOG_HEADER_SIZE, &payload_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(payload_off, payload_len, &crc_off))
        return NVLOG_ERR_BOUNDS;
    if (!nvlog_u32_add_checked(crc_off, sizeof(crc), &commit_off))
        return NVLOG_ERR_BOUNDS;

    uint8_t commit = 0xFFu;
    if (hal_write(ctx, commit_off, &commit, sizeof(commit)) != 0) return NVLOG_ERR_IO;
    if (hal_write(ctx, base, hdr_raw, sizeof(hdr_raw)) != 0) return NVLOG_ERR_IO;
    if (payload_len > 0)
        if (hal_write(ctx, payload_off, payload, payload_len) != 0)
            return NVLOG_ERR_IO;
    commit = 0x00u;
    if (hal_write(ctx, crc_off, &crc, sizeof(crc)) != 0)
        return NVLOG_ERR_IO;
    if (hal_write(ctx, commit_off, &commit, sizeof(commit)) != 0)
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
        while (it->count < ctx->record_count) {
            if (it->cursor != (uint32_t)NVLOG_REGION_HEADER_SIZE &&
                it->cursor + NVLOG_RECORD_OVERHEAD > ctx->region_size &&
                ctx->write_ptr < it->cursor) {
                it->cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
                continue;
            }
            nvlog_wire_rec_t hdr;
            uint32_t total = 0;
            uint32_t offset = it->cursor;
            int rc = verify_record(ctx, it->cursor, &hdr, &total);
            if (rc != 0)
                return (rc == NVLOG_ERR_NO_DATA) ? NVLOG_ERR_CORRUPT : (nvlog_status_t)rc;
            it->cursor += total;
            if (it->cursor >= ctx->region_size)
                it->cursor = (uint32_t)NVLOG_REGION_HEADER_SIZE;
            if (hdr.type != NVLOG_RECORD_TYPE_DATA)
                continue;
            out->seq    = hdr.seq;
            out->len    = (uint16_t)hdr.payload_len;
            out->offset = offset;
            it->count++;
            return NVLOG_OK;
        }
        return NVLOG_ERR_NO_DATA;

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
            out->len    = (uint16_t)hdr.payload_len;
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
        out->used_bytes = ctx->used_bytes;
        out->free_bytes = ctx->free_bytes;
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
    if (!ctx || !hal || !hal->read || !hal->write)
        return NVLOG_ERR_PARAM;
    if (region_size <= (uint32_t)NVLOG_REGION_HEADER_SIZE + NVLOG_RING_RESERVE_BYTES)
        return NVLOG_ERR_PARAM;
    return format_impl(ctx, hal, region_size, NVLOG_MODE_RING,
                       NVLOG_MEDIA_CLASS_BYTE_WRITABLE, 1u, 0xFFu, 0u);
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
    ctx->geometry_key = 0;
    ctx->media_class = 0;
    ctx->program_unit = 1u;
    ctx->erased_value = 0xFFu;

    nvlog_superblock_t sb;
    if (load_current_superblock(ctx, &sb) != 0) return NVLOG_ERR_CORRUPT;
    if (sb.mode != NVLOG_MODE_RING) return NVLOG_ERR_UNSUPPORTED;
    if (sb.region_size != region_size) return NVLOG_ERR_CORRUPT;
    ctx->generation = sb.generation;
    ctx->metadata_seq = sb.metadata_seq;
    ctx->write_ptr = sb.write_ptr;
    ctx->tail_ptr = sb.tail_ptr;
    ctx->next_seq = sb.next_seq;
    ctx->record_count = sb.record_count;
    ctx->used_bytes = sb.used_bytes;
    ctx->free_bytes = sb.free_bytes;
    ctx->padding_bytes = sb.padding_bytes;
    ctx->reserve_bytes = sb.reserve_bytes ? sb.reserve_bytes : NVLOG_RING_RESERVE_BYTES;
    ctx->ring_full = (ctx->free_bytes == 0);
    if (ctx->reserve_bytes != NVLOG_RING_RESERVE_BYTES) return NVLOG_ERR_UNSUPPORTED;
    if (ctx->write_ptr < NVLOG_REGION_HEADER_SIZE || ctx->write_ptr > region_size) return NVLOG_ERR_CORRUPT;
    if (ctx->tail_ptr < NVLOG_REGION_HEADER_SIZE || ctx->tail_ptr > region_size) return NVLOG_ERR_CORRUPT;
    if (ctx->free_bytes > ring_usable_bytes(ctx)) return NVLOG_ERR_CORRUPT;
    if (ring_validate_window(ctx) != 0) return NVLOG_ERR_CORRUPT;
    ctx->mounted = 1;
    return NVLOG_OK;
}

/* --- nvlog_ring_count ----------------------------------------- */

uint32_t nvlog_ring_count(nvlog_ctx_t *ctx)
{
    if (!ctx || !ctx->mounted || ctx->mode != NVLOG_MODE_RING) return 0;
    return ring_count_records(ctx);
}
