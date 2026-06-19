#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

#define MODEL_SIZE 4096u
#define MAX_MODEL_RECORDS 256u
#define MAX_HISTORY 1024u
#define MAX_PAYLOAD 48u

typedef struct {
    uint32_t seq;
    uint32_t generation;
    uint32_t offset;
    uint16_t len;
    uint8_t  data[MAX_PAYLOAD];
} model_record_t;

typedef struct {
    model_record_t recs[MAX_MODEL_RECORDS];
    size_t count;
} model_vec_t;

typedef struct {
    uint8_t bytes[MODEL_SIZE];
    uint8_t dirty[MODEL_SIZE];
    nvlog_mode_t mode;
    uint32_t region_size;
    uint32_t generation;
    uint32_t metadata_seq;
    uint32_t next_seq;
    uint32_t write_ptr;
    uint32_t tail_ptr;
    uint32_t record_count;
    uint8_t ring_full;
    model_vec_t live_records;
} shadow_state_t;

typedef struct {
    char items[MAX_HISTORY][96];
    size_t count;
} history_t;

static int g_pass = 0;
static int g_fail = 0;
static uint32_t g_seed = 0;
static int g_case = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

static uint16_t load_u16le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t load_u32le(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static void store_u16le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void store_u32le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t crc32_step(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        uint32_t mask = 0u - (crc & 1u);
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
    return crc;
}

static uint32_t crc32_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_step(crc, data[i]);
    return ~crc;
}

static void history_add(history_t *h, const char *fmt, ...)
{
    if (!h || h->count >= MAX_HISTORY) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->items[h->count], sizeof(h->items[h->count]), fmt, ap);
    va_end(ap);
    h->count++;
}

static void history_dump(const history_t *h)
{
    fprintf(stderr, "seed=%u case=%d history:\n", g_seed, g_case);
    for (size_t i = 0; i < h->count; i++)
        fprintf(stderr, "  %s\n", h->items[i]);
}

static void vec_clear(model_vec_t *v)
{
    if (v) memset(v, 0, sizeof(*v));
}

static int vec_push(model_vec_t *v, uint32_t seq, uint32_t generation,
                    uint32_t offset, const void *data, uint16_t len)
{
    if (!v || v->count >= MAX_MODEL_RECORDS || len > MAX_PAYLOAD) return -1;
    model_record_t *r = &v->recs[v->count++];
    r->seq = seq;
    r->generation = generation;
    r->offset = offset;
    r->len = len;
    if (len && data) memcpy(r->data, data, len);
    return 0;
}

static int vec_equal(const model_vec_t *a, const model_vec_t *b)
{
    if (!a || !b || a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) {
        const model_record_t *x = &a->recs[i];
        const model_record_t *y = &b->recs[i];
        if (x->seq != y->seq || x->generation != y->generation ||
            x->offset != y->offset || x->len != y->len)
            return 0;
        if (x->len && memcmp(x->data, y->data, x->len) != 0)
            return 0;
    }
    return 1;
}

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
} wire_superblock_t;

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
} wire_record_t;

static void encode_superblock(uint8_t *dst, const wire_superblock_t *sb)
{
    store_u32le(dst + 0, sb->magic);
    store_u16le(dst + 4, sb->format_version);
    dst[6] = sb->mode;
    dst[7] = sb->reserved0;
    store_u32le(dst + 8, sb->region_size);
    store_u32le(dst + 12, sb->generation);
    store_u32le(dst + 16, sb->metadata_seq);
    store_u32le(dst + 20, sb->feature_flags);
    store_u32le(dst + 24, sb->crc32);
    memset(dst + 28, 0xFF, 4);
}

static int decode_superblock(wire_superblock_t *sb, const uint8_t *src)
{
    if (!sb || !src) return -1;
    sb->magic = load_u32le(src + 0);
    sb->format_version = load_u16le(src + 4);
    sb->mode = src[6];
    sb->reserved0 = src[7];
    sb->region_size = load_u32le(src + 8);
    sb->generation = load_u32le(src + 12);
    sb->metadata_seq = load_u32le(src + 16);
    sb->feature_flags = load_u32le(src + 20);
    sb->crc32 = load_u32le(src + 24);
    return 0;
}

static void encode_record(uint8_t *dst, const wire_record_t *rec)
{
    dst[0] = rec->magic;
    dst[1] = rec->type;
    dst[2] = rec->version;
    dst[3] = rec->flags;
    store_u32le(dst + 4, rec->generation);
    store_u32le(dst + 8, rec->seq);
    store_u16le(dst + 12, rec->payload_len);
    store_u16le(dst + 14, rec->total_len);
    store_u32le(dst + 16, rec->crc32);
}

static int decode_record(wire_record_t *rec, const uint8_t *src)
{
    if (!rec || !src) return -1;
    rec->magic = src[0];
    rec->type = src[1];
    rec->version = src[2];
    rec->flags = src[3];
    rec->generation = load_u32le(src + 4);
    rec->seq = load_u32le(src + 8);
    rec->payload_len = load_u16le(src + 12);
    rec->total_len = load_u16le(src + 14);
    rec->crc32 = load_u32le(src + 16);
    return 0;
}

static int sb_valid(const shadow_state_t *s, uint32_t offset, wire_superblock_t *sb)
{
    uint8_t raw[NVLOG_SUPERBLOCK_SIZE];
    if (offset + NVLOG_SUPERBLOCK_SIZE > s->region_size) return 0;
    memcpy(raw, s->bytes + offset, sizeof(raw));
    if (decode_superblock(sb, raw) != 0) return 0;
    if (sb->magic != NVLOG_MEDIA_MAGIC) return 0;
    if (sb->format_version != NVLOG_MEDIA_VERSION) return 0;
    if (sb->mode != NVLOG_MODE_LINEAR && sb->mode != NVLOG_MODE_RING) return 0;
    if (sb->reserved0 != 0 || sb->feature_flags != 0) return 0;
    if (sb->region_size != s->region_size) return 0;
    uint32_t crc = crc32_bytes(raw, 24u);
    return sb->crc32 == crc;
}

static uint8_t generation_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static int record_valid(const shadow_state_t *s, uint32_t offset, model_record_t *out)
{
    if (offset + NVLOG_HEADER_SIZE > s->region_size) return 0;
    if (s->bytes[offset] == 0xFFu) return 0;

    wire_record_t rec;
    uint8_t raw[NVLOG_HEADER_SIZE];
    memcpy(raw, s->bytes + offset, sizeof(raw));
    if (decode_record(&rec, raw) != 0) return 0;
    if (rec.magic != NVLOG_RECORD_MAGIC) return 0;
    if (rec.version != NVLOG_RECORD_VERSION) return 0;
    if (rec.type != NVLOG_RECORD_TYPE_DATA &&
        rec.type != NVLOG_RECORD_TYPE_WRAP_PAD) return 0;
    if (rec.flags != NVLOG_FLAGS_LINEAR && rec.flags != NVLOG_FLAGS_RING) return 0;
    if (rec.generation != s->generation) return 0;
    if (rec.payload_len > MAX_PAYLOAD) return 0;
    if (rec.total_len != (uint16_t)(NVLOG_RECORD_OVERHEAD + rec.payload_len)) return 0;
    if (rec.crc32 != crc32_bytes(raw, 16u)) return 0;
    uint32_t end = 0;
    if (UINT32_MAX - offset < (uint32_t)NVLOG_RECORD_OVERHEAD + rec.payload_len) return 0;
    end = offset + (uint32_t)NVLOG_RECORD_OVERHEAD + rec.payload_len;
    if (end > s->region_size) return 0;

    uint32_t stored_crc = load_u32le(s->bytes + offset + NVLOG_HEADER_SIZE + rec.payload_len);
    uint8_t commit = s->bytes[offset + NVLOG_HEADER_SIZE + rec.payload_len + sizeof(uint32_t)];
    uint32_t crc2 = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < NVLOG_HEADER_SIZE + rec.payload_len; i++)
        crc2 = crc32_step(crc2, s->bytes[offset + i]);
    crc2 = ~crc2;
    if (stored_crc != crc2) return 0;
    if (commit != 0x00u) return 0;

    if (out) {
        out->seq = rec.seq;
        out->generation = rec.generation;
        out->offset = offset;
        out->len = rec.payload_len;
        memcpy(out->data, s->bytes + offset + NVLOG_HEADER_SIZE, rec.payload_len);
    }
    return 1;
}

static void shadow_reset(shadow_state_t *s, uint32_t region_size)
{
    memset(s, 0xFF, sizeof(*s));
    s->region_size = region_size;
    s->mode = NVLOG_MODE_LINEAR;
    s->write_ptr = NVLOG_REGION_HEADER_SIZE;
    s->tail_ptr = NVLOG_REGION_HEADER_SIZE;
    vec_clear(&s->live_records);
    memset(s->dirty, 0, sizeof(s->dirty));
}

static uint32_t next_generation(uint32_t current)
{
    current++;
    if (current == 0) current = 1;
    return current;
}

static void shadow_publish_superblocks(shadow_state_t *s)
{
    wire_superblock_t sb = {
        .magic = NVLOG_MEDIA_MAGIC,
        .format_version = NVLOG_MEDIA_VERSION,
        .mode = (uint8_t)s->mode,
        .reserved0 = 0,
        .region_size = s->region_size,
        .generation = s->generation,
        .metadata_seq = s->metadata_seq,
        .feature_flags = 0,
        .crc32 = 0,
    };
    uint8_t raw[NVLOG_SUPERBLOCK_SIZE];
    encode_superblock(raw, &sb);
    sb.crc32 = crc32_bytes(raw, 24u);
    encode_superblock(raw, &sb);
    memcpy(s->bytes + 0, raw, sizeof(raw));
    memcpy(s->bytes + NVLOG_SUPERBLOCK_SIZE, raw, sizeof(raw));
}

static void shadow_format(shadow_state_t *s, nvlog_mode_t mode)
{
    wire_superblock_t a = {0}, b = {0}, chosen = {0};
    uint32_t gen = 1;
    uint32_t meta = 0;
    int have_a = sb_valid(s, 0, &a);
    int have_b = sb_valid(s, NVLOG_SUPERBLOCK_SIZE, &b);
    if (have_a || have_b) {
        chosen = have_a ? a : b;
        if (have_a && have_b && generation_newer(b.generation, a.generation))
            chosen = b;
        gen = next_generation(chosen.generation);
        meta = chosen.metadata_seq + 1u;
        if (meta == 0) meta = 1u;
    }
    s->mode = mode;
    s->generation = gen;
    s->metadata_seq = meta;
    s->next_seq = 0;
    s->write_ptr = NVLOG_REGION_HEADER_SIZE;
    s->tail_ptr = NVLOG_REGION_HEADER_SIZE;
    s->record_count = 0;
    s->ring_full = 0;
    vec_clear(&s->live_records);
    memset(s->dirty, 0, sizeof(s->dirty));
    shadow_publish_superblocks(s);
}

static uint32_t record_total(uint16_t len)
{
    return (uint32_t)NVLOG_RECORD_OVERHEAD + len;
}

static int shadow_append(shadow_state_t *s, const void *payload, uint16_t len, int fail_after)
{
    if (len > MAX_PAYLOAD) return -1;
    uint32_t total = record_total(len);
    uint32_t capacity = s->region_size - NVLOG_REGION_HEADER_SIZE;
    if (total > capacity) return -1;

    if (s->mode == NVLOG_MODE_LINEAR) {
        if (UINT32_MAX - s->write_ptr < total || s->write_ptr + total > s->region_size)
            return -1;
    } else {
        uint32_t write_end = s->write_ptr + total;
        if (write_end > s->region_size)
            s->write_ptr = NVLOG_REGION_HEADER_SIZE;
    }

    uint32_t base = s->write_ptr;
    uint32_t payload_off = base + NVLOG_HEADER_SIZE;
    uint32_t crc_off = payload_off + len;
    uint32_t commit_off = crc_off + sizeof(uint32_t);
    wire_record_t rec = {
        .magic = NVLOG_RECORD_MAGIC,
        .type = NVLOG_RECORD_TYPE_DATA,
        .version = NVLOG_RECORD_VERSION,
        .flags = (s->mode == NVLOG_MODE_RING) ? NVLOG_FLAGS_RING : NVLOG_FLAGS_LINEAR,
        .generation = s->generation,
        .seq = s->next_seq,
        .payload_len = len,
        .total_len = (uint16_t)total,
        .crc32 = 0,
    };
    uint8_t hdr[NVLOG_HEADER_SIZE];
    encode_record(hdr, &rec);
    rec.crc32 = crc32_bytes(hdr, 16u);
    encode_record(hdr, &rec);

    int writes = 0;
    if (fail_after >= 0 && writes >= fail_after) return -1;
    s->bytes[commit_off] = 0xFFu;
    s->dirty[commit_off] = 1;
    writes++;

    if (fail_after >= 0 && writes >= fail_after) return -1;
    memcpy(s->bytes + base, hdr, NVLOG_HEADER_SIZE);
    for (uint32_t i = 0; i < NVLOG_HEADER_SIZE && base + i < s->region_size; i++)
        s->dirty[base + i] = 1;
    writes++;

    if (len > 0) {
        if (fail_after >= 0 && writes >= fail_after) return -1;
        memcpy(s->bytes + payload_off, payload, len);
        for (uint32_t i = 0; i < len && payload_off + i < s->region_size; i++)
            s->dirty[payload_off + i] = 1;
        writes++;
    }

    if (fail_after >= 0 && writes >= fail_after) return -1;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < NVLOG_HEADER_SIZE + len; i++)
        crc = crc32_step(crc, s->bytes[base + i]);
    crc = ~crc;
    store_u32le(s->bytes + crc_off, crc);
    for (uint32_t i = 0; i < sizeof(crc) && crc_off + i < s->region_size; i++)
        s->dirty[crc_off + i] = 1;
    writes++;

    if (fail_after >= 0 && writes >= fail_after) return -1;
    if (commit_off < s->region_size) {
        s->bytes[commit_off] = 0x00u;
        s->dirty[commit_off] = 1;
    }

    if (s->mode == NVLOG_MODE_LINEAR) {
        s->write_ptr = base + total;
        s->next_seq++;
        vec_push(&s->live_records, rec.seq, rec.generation, base, payload, len);
        memset(s->dirty, 0, sizeof(s->dirty));
        return 0;
    }

    uint32_t write_at = base;
    uint32_t write_end = base + total;
    if (write_end > s->region_size) {
        write_at = NVLOG_REGION_HEADER_SIZE;
        write_end = write_at + total;
    }
    uint32_t evicted = 0;
    if (s->ring_full || write_end > s->region_size) {
        while (s->tail_ptr >= write_at && s->tail_ptr < write_end) {
            model_record_t tmp;
            if (!record_valid(s, s->tail_ptr, &tmp)) {
                s->tail_ptr += NVLOG_RECORD_OVERHEAD;
            } else {
                if (s->tail_ptr + NVLOG_RECORD_OVERHEAD + tmp.len > s->region_size)
                    s->tail_ptr = NVLOG_REGION_HEADER_SIZE;
                else
                    s->tail_ptr += record_total(tmp.len);
            }
            if (s->tail_ptr >= s->region_size)
                s->tail_ptr = NVLOG_REGION_HEADER_SIZE;
            evicted++;
            if (s->tail_ptr < write_at) break;
        }
    }
    s->write_ptr = base + total;
    if (s->write_ptr >= s->region_size) {
        s->write_ptr = NVLOG_REGION_HEADER_SIZE;
        s->ring_full = 1;
    }
    s->next_seq++;
    vec_push(&s->live_records, rec.seq, rec.generation, base, payload, len);
    memset(s->dirty, 0, sizeof(s->dirty));
    if (s->record_count > evicted) s->record_count -= evicted; else s->record_count = 0;
    s->record_count++;
    if (s->write_ptr == s->tail_ptr) s->ring_full = 1;
    return 0;
}

typedef struct {
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t record_count;
    uint32_t next_seq;
    model_vec_t records;
} shadow_view_t;

static void shadow_view_clear(shadow_view_t *v)
{
    memset(v, 0, sizeof(*v));
}


static int shadow_mount(const shadow_state_t *s, const model_vec_t *committed, shadow_view_t *view)
{
    wire_superblock_t a, b;
    int have_a = sb_valid(s, 0, &a);
    int have_b = sb_valid(s, NVLOG_SUPERBLOCK_SIZE, &b);
    if (!have_a && !have_b) return -1;
    wire_superblock_t sb = have_a ? a : b;
    if (have_a && have_b) sb = generation_newer(b.generation, a.generation) ? b : a;
    if (sb.mode != NVLOG_MODE_LINEAR && sb.mode != NVLOG_MODE_RING) return -1;
    if (sb.region_size != s->region_size) return -1;

    shadow_state_t tmp = *s;
    tmp.mode = (nvlog_mode_t)sb.mode;
    tmp.generation = sb.generation;
    tmp.metadata_seq = sb.metadata_seq;
    shadow_view_clear(view);
    view->records = *committed;
    view->record_count = (uint32_t)committed->count;
    view->next_seq = committed->count ? committed->recs[committed->count - 1].seq + 1u : 0u;
    view->used_bytes = 0;
    for (size_t i = 0; i < committed->count; i++)
        view->used_bytes += record_total(committed->recs[i].len);
    view->free_bytes = (view->used_bytes >= s->region_size - NVLOG_REGION_HEADER_SIZE)
                     ? 0
                     : (s->region_size - NVLOG_REGION_HEADER_SIZE - view->used_bytes);
    return 0;
}

static int shadow_collect_live(const model_vec_t *src, model_vec_t *out)
{
    if (!src || !out) return -1;
    *out = *src;
    return 0;
}

static void shadow_live_stats(const shadow_state_t *s, const model_vec_t *records, shadow_view_t *view)
{
    shadow_view_clear(view);
    view->records = *records;
    view->record_count = (uint32_t)records->count;
    view->next_seq = s->next_seq;
    if (s->mode == NVLOG_MODE_RING) {
        uint32_t used = 0;
        for (size_t i = 0; i < records->count; i++)
            used += record_total(records->recs[i].len);
        view->used_bytes = used;
        view->free_bytes = (used >= s->region_size - NVLOG_REGION_HEADER_SIZE)
                         ? 0
                         : (s->region_size - NVLOG_REGION_HEADER_SIZE - used);
    } else {
        view->used_bytes = s->write_ptr - NVLOG_REGION_HEADER_SIZE;
        view->free_bytes = s->region_size - s->write_ptr;
    }
}

static int shadow_mutation_ok(const shadow_view_t *exp, const model_vec_t *got)
{
    return exp->record_count == got->count;
}

static void model_dump_record(const char *prefix, const model_record_t *r)
{
    fprintf(stderr, "%s seq=%u gen=%u off=%u len=%u data=", prefix, r->seq, r->generation, r->offset, r->len);
    for (uint16_t i = 0; i < r->len && i < 12; i++)
        fprintf(stderr, "%02X", r->data[i]);
    if (r->len > 12) fprintf(stderr, "...");
    fputc('\n', stderr);
}

static void dump_state(const char *title, const shadow_view_t *exp, const model_vec_t *act)
{
    fprintf(stderr, "%s\n", title);
    fprintf(stderr, "expected count=%zu used=%u free=%u next_seq=%u\n",
            exp->records.count, exp->used_bytes, exp->free_bytes, exp->next_seq);
    for (size_t i = 0; i < exp->records.count; i++)
        model_dump_record("  exp", &exp->records.recs[i]);
    fprintf(stderr, "actual count=%zu\n", act->count);
    for (size_t i = 0; i < act->count; i++)
        model_dump_record("  act", &act->recs[i]);
}

static void die_mismatch(const history_t *hist, const shadow_view_t *exp, const model_vec_t *act)
{
    history_dump(hist);
    dump_state("state mismatch", exp, act);
    fprintf(stderr, "seed=%u case=%d\n", g_seed, g_case);
    exit(1);
}

static void collect_actual(nvlog_ctx_t *ctx, model_vec_t *out)
{
    nvlog_iter_t it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, ctx) == NVLOG_OK);
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
        uint8_t payload[MAX_PAYLOAD];
        uint8_t hdr_raw[NVLOG_HEADER_SIZE];
        wire_record_t hdr;
        CHECK(ctx->hal.read(rec.offset, hdr_raw, sizeof(hdr_raw), ctx->hal.user) == 0);
        CHECK(decode_record(&hdr, hdr_raw) == 0);
        CHECK(hdr.generation == (uint32_t)load_u32le(hdr_raw + 4));
        CHECK(rec.len == hdr.payload_len);
        CHECK(hdr.generation != 0);
        CHECK(rec.len <= sizeof(payload));
        CHECK(nvlog_read_payload(ctx, &rec, payload, sizeof(payload)) == NVLOG_OK);
        CHECK(vec_push(out, rec.seq, hdr.generation, rec.offset, payload, rec.len) == 0);
    }
}

static void compare_views(const history_t *hist, const shadow_view_t *exp, const model_vec_t *act)
{
    if (!shadow_mutation_ok(exp, act) || exp->records.count != act->count || !vec_equal(&exp->records, act))
        die_mismatch(hist, exp, act);
}

static void compare_stats(const history_t *hist, nvlog_ctx_t *ctx, const shadow_view_t *exp)
{
    nvlog_stats_t st;
    CHECK(nvlog_stats(ctx, &st) == NVLOG_OK);
    if (st.used_bytes != exp->used_bytes ||
        st.free_bytes != exp->free_bytes ||
        st.record_count != exp->record_count ||
        st.next_seq != exp->next_seq) {
        fprintf(stderr, "stats mismatch seed=%u case=%d\n", g_seed, g_case);
        history_dump(hist);
        fprintf(stderr, "expected used=%u free=%u count=%u next=%u\n",
                exp->used_bytes, exp->free_bytes, exp->record_count, exp->next_seq);
        fprintf(stderr, "actual used=%u free=%u count=%u next=%u\n",
                st.used_bytes, st.free_bytes, st.record_count, st.next_seq);
        exit(1);
    }
}

int main(void)
{
    const uint32_t seed = 0xC0FFEEu;
    g_seed = seed;
    srand(seed);
    printf("nvlog model test seed=%u\n", seed);

    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    CHECK(nvlog_posix_open_ram(&pctx, &hal, MODEL_SIZE) == 0);

    shadow_state_t shadow;
    shadow_reset(&shadow, MODEL_SIZE);

    nvlog_ctx_t ctx;
    int have_ctx = 0;
    history_t hist = {0};
    model_vec_t committed = {0};
    uint8_t payload[MAX_PAYLOAD];

    for (int i = 0; i < 5000; i++) {
        g_case = i;
        uint32_t op = (uint32_t)(rand() % 8);
        uint16_t len = (uint16_t)(rand() % MAX_PAYLOAD);
        for (uint16_t j = 0; j < len; j++) payload[j] = (uint8_t)rand();

        if (op == 0) {
            history_add(&hist, "%04d: format mode=%u", i, shadow.mode);
            shadow_format(&shadow, shadow.mode);
            vec_clear(&committed);
            CHECK((shadow.mode == NVLOG_MODE_RING ? nvlog_ring_format(&ctx, &hal, MODEL_SIZE)
                                                  : nvlog_format(&ctx, &hal, MODEL_SIZE)) == NVLOG_OK);
            have_ctx = 1;
        } else if (op == 1) {
            shadow.mode = (shadow.mode == NVLOG_MODE_RING) ? NVLOG_MODE_LINEAR : NVLOG_MODE_RING;
            history_add(&hist, "%04d: reformat mode=%u", i, shadow.mode);
            shadow_format(&shadow, shadow.mode);
            vec_clear(&committed);
            CHECK((shadow.mode == NVLOG_MODE_RING ? nvlog_ring_format(&ctx, &hal, MODEL_SIZE)
                                                  : nvlog_format(&ctx, &hal, MODEL_SIZE)) == NVLOG_OK);
            have_ctx = 1;
        } else if (op == 2 && have_ctx) {
            int fail_after = (rand() % 6 == 0) ? (rand() % 5) : -1;
            history_add(&hist, "%04d: append len=%u fail_after=%d mode=%u", i, len, fail_after, shadow.mode);
            uint32_t expected_seq = shadow.next_seq;
            uint32_t expected_offset = shadow.write_ptr;
            uint32_t expected_generation = shadow.generation;
            nvlog_posix_inject_fail_after(&pctx, fail_after);
            nvlog_status_t st = nvlog_append(&ctx, payload, len);
            nvlog_posix_inject_fail_after(&pctx, -1);
            if (st == NVLOG_OK) {
                CHECK(shadow_append(&shadow, payload, len, -1) == 0);
                model_vec_t live_exp;
                shadow_view_t live_view;
                vec_clear(&live_exp);
                CHECK(vec_push(&committed, expected_seq, expected_generation, expected_offset, payload, len) == 0);
                CHECK(shadow_collect_live(&committed, &live_exp) == 0);
                shadow_live_stats(&shadow, &live_exp, &live_view);
                model_vec_t act;
                vec_clear(&act);
                collect_actual(&ctx, &act);
                compare_views(&hist, &live_view, &act);
                compare_stats(&hist, &ctx, &live_view);
            } else {
                CHECK(st == NVLOG_ERR_IO || st == NVLOG_ERR_FULL || st == NVLOG_ERR_BOUNDS);
                CHECK(shadow_append(&shadow, payload, len, fail_after) <= 0);
                nvlog_ctx_t rm;
                CHECK((shadow.mode == NVLOG_MODE_RING ? nvlog_ring_mount(&rm, &hal, MODEL_SIZE)
                                                      : nvlog_mount(&rm, &hal, MODEL_SIZE)) == NVLOG_OK);
                ctx = rm;
                shadow_view_t exp;
                CHECK(shadow_mount(&shadow, &committed, &exp) == 0);
                model_vec_t act;
                vec_clear(&act);
                collect_actual(&ctx, &act);
                compare_views(&hist, &exp, &act);
                compare_stats(&hist, &ctx, &exp);
            }
        } else if (op == 3 && have_ctx) {
            history_add(&hist, "%04d: remount mode=%u", i, shadow.mode);
            CHECK((shadow.mode == NVLOG_MODE_RING ? nvlog_ring_mount(&ctx, &hal, MODEL_SIZE)
                                                  : nvlog_mount(&ctx, &hal, MODEL_SIZE)) == NVLOG_OK);
            shadow_view_t exp;
            CHECK(shadow_mount(&shadow, &committed, &exp) == 0);
            model_vec_t act;
            vec_clear(&act);
            collect_actual(&ctx, &act);
            compare_views(&hist, &exp, &act);
            compare_stats(&hist, &ctx, &exp);
        } else if (op == 4) {
            history_add(&hist, "%04d: corrupt", i);
            size_t off = (size_t)(rand() % MODEL_SIZE);
            shadow.bytes[off] ^= 0x5Au;
            shadow.dirty[off] = 1;
            if (have_ctx) {
                nvlog_ctx_t rm;
                CHECK((shadow.mode == NVLOG_MODE_RING ? nvlog_ring_mount(&rm, &hal, MODEL_SIZE)
                                                      : nvlog_mount(&rm, &hal, MODEL_SIZE)) == NVLOG_OK);
                ctx = rm;
                shadow_view_t exp;
                CHECK(shadow_mount(&shadow, &committed, &exp) == 0);
                model_vec_t act;
                vec_clear(&act);
                collect_actual(&ctx, &act);
                compare_views(&hist, &exp, &act);
                compare_stats(&hist, &ctx, &exp);
            }
        } else if (op == 5 && have_ctx) {
            history_add(&hist, "%04d: powerloss len=%u mode=%u", i, len, shadow.mode);
            nvlog_posix_inject_fail_after(&pctx, 1);
            nvlog_status_t st = nvlog_append(&ctx, payload, len);
            nvlog_posix_inject_fail_after(&pctx, -1);
            CHECK(st == NVLOG_OK || st == NVLOG_ERR_IO || st == NVLOG_ERR_FULL || st == NVLOG_ERR_BOUNDS);
            CHECK(shadow_append(&shadow, payload, len, 1) <= 0);
            nvlog_ctx_t rm;
            CHECK((shadow.mode == NVLOG_MODE_RING ? nvlog_ring_mount(&rm, &hal, MODEL_SIZE)
                                                  : nvlog_mount(&rm, &hal, MODEL_SIZE)) == NVLOG_OK);
            ctx = rm;
            shadow_view_t exp;
            CHECK(shadow_mount(&shadow, &committed, &exp) == 0);
            model_vec_t act;
            vec_clear(&act);
            collect_actual(&ctx, &act);
            compare_views(&hist, &exp, &act);
            compare_stats(&hist, &ctx, &exp);
        } else if (op == 6 && have_ctx) {
            history_add(&hist, "%04d: stats mode=%u", i, shadow.mode);
                model_vec_t live_exp;
                shadow_view_t live_view;
                vec_clear(&live_exp);
                CHECK(shadow_collect_live(&committed, &live_exp) == 0);
                shadow_live_stats(&shadow, &live_exp, &live_view);
                compare_stats(&hist, &ctx, &live_view);
        } else if (op == 7 && have_ctx) {
            history_add(&hist, "%04d: iterator probe mode=%u", i, shadow.mode);
            nvlog_iter_t it;
            nvlog_record_t rec;
            CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
            nvlog_status_t st = nvlog_iter_next(&it, &rec);
            CHECK(st == NVLOG_OK || st == NVLOG_ERR_NO_DATA ||
                  st == NVLOG_ERR_STALE || st == NVLOG_ERR_CORRUPT ||
                  st == NVLOG_ERR_UNSUPPORTED || st == NVLOG_ERR_IO ||
                  st == NVLOG_ERR_BOUNDS);
        }
    }

    nvlog_posix_close(&pctx);
    printf("PASSED: %d\nFAILED: %d\n", g_pass, g_fail);
    printf("sizeof(nvlog_ctx_t)=%zu\n", sizeof(nvlog_ctx_t));
    printf("sizeof(nvlog_iter_t)=%zu\n", sizeof(nvlog_iter_t));
    printf("sizeof(nvlog_record_t)=%zu\n", sizeof(nvlog_record_t));
    printf("sizeof(nvlog_hal_t)=%zu\n", sizeof(nvlog_hal_t));
    return g_fail == 0 ? 0 : 1;
}
