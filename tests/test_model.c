#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"
#include "../backends/nvlog_flash_sim.h"
#include "../include/nvlog_hal_flash.h"

#define MODEL_SIZE 131072u
#define MAX_MODEL_RECORDS 4096u
#define MAX_HISTORY 128u
#define MAX_PAYLOAD 64u
#define SEED_COUNT 3u
#define OPS_PER_SEED 10000u

typedef struct {
    uint32_t seq;
    uint32_t len;
    uint32_t offset;
    uint8_t  payload[MAX_PAYLOAD];
} shadow_record_t;

typedef struct {
    shadow_record_t recs[MAX_MODEL_RECORDS];
    size_t count;
    uint8_t bytes[MODEL_SIZE];
    uint8_t mounted;
    uint8_t corrupted;
    uint32_t generation;
    uint32_t metadata_seq;
    uint32_t next_seq;
    uint32_t write_ptr;
    uint32_t tail_ptr;
    uint32_t record_count;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t padding_bytes;
    uint32_t reserve_bytes;
    uint32_t region_size;
} shadow_state_t;

typedef struct {
    char items[MAX_HISTORY][128];
    size_t count;
} history_t;

static int g_pass = 0;
static int g_fail = 0;
static uint32_t g_seed = 0;
static uint32_t g_op_index = 0;
static uint32_t g_scenarios = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

static uint32_t rng_step(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void history_add(history_t *h, const char *fmt, ...)
{
    va_list ap;
    if (h->count < MAX_HISTORY) {
        va_start(ap, fmt);
        vsnprintf(h->items[h->count], sizeof(h->items[h->count]), fmt, ap);
        va_end(ap);
        h->count++;
    } else {
        memmove(h->items, h->items + 1, (MAX_HISTORY - 1u) * sizeof(h->items[0]));
        va_start(ap, fmt);
        vsnprintf(h->items[MAX_HISTORY - 1u], sizeof(h->items[0]), fmt, ap);
        va_end(ap);
    }
}

static void history_dump(const history_t *h)
{
    fprintf(stderr, "recent history:\n");
    for (size_t i = 0; i < h->count; i++)
        fprintf(stderr, "  %s\n", h->items[i]);
}

static void shadow_reset(shadow_state_t *s)
{
    memset(s, 0, sizeof(*s));
    memset(s->bytes, 0xFF, sizeof(s->bytes));
    s->generation = 1;
    s->metadata_seq = 1;
    s->next_seq = 0;
    s->write_ptr = NVLOG_REGION_HEADER_SIZE;
    s->tail_ptr = NVLOG_REGION_HEADER_SIZE;
    s->reserve_bytes = NVLOG_RECORD_OVERHEAD;
    s->region_size = MODEL_SIZE;
    s->free_bytes = MODEL_SIZE - NVLOG_REGION_HEADER_SIZE;
}

static uint32_t rec_total(uint32_t len)
{
    return (uint32_t)(NVLOG_RECORD_OVERHEAD + len);
}

static void shadow_recalculate(shadow_state_t *s)
{
    uint32_t used = 0;
    for (size_t i = 0; i < s->count; i++)
        used += rec_total(s->recs[i].len);
    s->record_count = (uint32_t)s->count;
    s->used_bytes = used;
    s->free_bytes = (s->region_size - NVLOG_REGION_HEADER_SIZE > used)
                  ? (s->region_size - NVLOG_REGION_HEADER_SIZE - used)
                  : 0;
}

static void shadow_format(shadow_state_t *s)
{
    shadow_reset(s);
}

static void shadow_append(shadow_state_t *s, const uint8_t *payload, uint32_t len)
{
    uint32_t total = rec_total(len);
    uint32_t usable = s->region_size - NVLOG_REGION_HEADER_SIZE - s->reserve_bytes;
    if (len > MAX_PAYLOAD || total > usable) return;

    while (s->count > 0 && s->used_bytes + total > usable) {
        uint32_t old_total = rec_total(s->recs[0].len);
        if (old_total <= s->used_bytes)
            s->used_bytes -= old_total;
        memmove(s->recs, s->recs + 1, (s->count - 1u) * sizeof(s->recs[0]));
        s->count--;
        s->tail_ptr = (s->count > 0) ? s->recs[0].offset : s->write_ptr;
    }

    if (s->write_ptr + total > s->region_size)
        s->write_ptr = NVLOG_REGION_HEADER_SIZE;

    if (s->count >= MAX_MODEL_RECORDS)
        return;

    shadow_record_t *r = &s->recs[s->count++];
    r->seq = s->next_seq++;
    r->len = len;
    r->offset = s->write_ptr;
    memset(r->payload, 0, sizeof(r->payload));
    if (len > 0 && payload)
        memcpy(r->payload, payload, len);

    if (r->offset + total <= MODEL_SIZE) {
        memset(s->bytes + r->offset, 0x00, total);
        if (len > 0)
            memcpy(s->bytes + r->offset + NVLOG_RECORD_HEADER_SIZE, r->payload, len);
    }

    s->write_ptr += total;
    if (s->write_ptr >= s->region_size)
        s->write_ptr = NVLOG_REGION_HEADER_SIZE;
    s->tail_ptr = (s->count > 0) ? s->recs[0].offset : s->write_ptr;
    shadow_recalculate(s);
}

static void compare_records(const history_t *h, const shadow_state_t *exp, const shadow_record_t *act, uint32_t n)
{
    CHECK(n == exp->count);
    if (n != exp->count) {
        history_dump(h);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        CHECK(act[i].seq == exp->recs[i].seq);
        CHECK(act[i].len == exp->recs[i].len);
        CHECK(memcmp(act[i].payload, exp->recs[i].payload, exp->recs[i].len) == 0);
    }
}

static void collect_actual(nvlog_ctx_t *ctx, shadow_record_t *out, uint32_t max, uint32_t *count_out)
{
    nvlog_iter_t it;
    nvlog_record_t rec;
    uint32_t n = 0;
    CHECK(nvlog_iter_init(&it, ctx) == NVLOG_OK);
    while (n < max) {
        nvlog_status_t st = nvlog_iter_next(&it, &rec);
        if (st == NVLOG_ERR_NO_DATA)
            break;
        CHECK(st == NVLOG_OK);
        out[n].seq = rec.seq;
        out[n].len = rec.len;
        out[n].offset = rec.offset;
        if (rec.len > 0)
            CHECK(nvlog_read_payload(ctx, &rec, out[n].payload, rec.len) == NVLOG_OK);
        n++;
    }
    *count_out = n;
}

static void compare_stats(const history_t *h, nvlog_ctx_t *ctx, const shadow_state_t *exp)
{
    nvlog_stats_t st;
    CHECK(nvlog_stats(ctx, &st) == NVLOG_OK);
    if (st.used_bytes != exp->used_bytes ||
        st.free_bytes != exp->free_bytes ||
        st.record_count != exp->record_count ||
        st.next_seq != exp->next_seq) {
        fprintf(stderr, "stats mismatch seed=%u op=%u\n", g_seed, g_op_index);
        history_dump(h);
        fprintf(stderr, "expected used=%u free=%u count=%u next=%u\n",
                exp->used_bytes, exp->free_bytes, exp->record_count, exp->next_seq);
        fprintf(stderr, "actual used=%u free=%u count=%u next=%u\n",
                st.used_bytes, st.free_bytes, st.record_count, st.next_seq);
        g_fail++;
    } else {
        g_pass += 4;
    }
}

static void compare_state(const history_t *h, nvlog_ctx_t *ctx, const shadow_state_t *exp)
{
    shadow_record_t act[MAX_MODEL_RECORDS];
    uint32_t act_n = 0;
    collect_actual(ctx, act, MAX_MODEL_RECORDS, &act_n);
    compare_records(h, exp, act, act_n);
    compare_stats(h, ctx, exp);
}

static void run_seed(uint32_t seed)
{
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    shadow_state_t shadow;
    nvlog_ctx_t ctx;
    history_t hist = {0};
    uint8_t payload[MAX_PAYLOAD];
    uint32_t rng = seed ? seed : 1u;
    g_seed = seed;
    CHECK(nvlog_posix_open_ram(&pctx, &hal, MODEL_SIZE) == 0);
    shadow_format(&shadow);
    CHECK(nvlog_ring_format(&ctx, &hal, MODEL_SIZE) == NVLOG_OK);

    for (uint32_t i = 0; i < OPS_PER_SEED; i++) {
        g_op_index = i;
        g_scenarios++;
        uint32_t op = rng_step(&rng) % 8u;
        uint32_t len = rng_step(&rng) % (MAX_PAYLOAD + 1u);
        for (uint32_t j = 0; j < len; j++)
            payload[j] = (uint8_t)(rng_step(&rng) & 0xFFu);

        if (op == 0u) {
            history_add(&hist, "%u: format", i);
            shadow_format(&shadow);
            CHECK(nvlog_ring_format(&ctx, &hal, MODEL_SIZE) == NVLOG_OK);
            compare_state(&hist, &ctx, &shadow);
        } else if (op == 1u) {
            history_add(&hist, "%u: append len=%u", i, len);
            nvlog_posix_inject_fail_after(&pctx, (int32_t)(rng_step(&rng) % 3u));
            nvlog_status_t st = nvlog_append(&ctx, payload, (uint16_t)len);
            nvlog_posix_inject_fail_after(&pctx, -1);
            if (st == NVLOG_OK)
                shadow_append(&shadow, payload, len);
            CHECK(nvlog_ring_mount(&ctx, &hal, MODEL_SIZE) == NVLOG_OK);
            compare_state(&hist, &ctx, &shadow);
        } else if (op == 2u) {
            history_add(&hist, "%u: iterate", i);
            compare_state(&hist, &ctx, &shadow);
        } else if (op == 3u) {
            history_add(&hist, "%u: payload", i);
            if (shadow.count > 0) {
                uint32_t idx = rng_step(&rng) % (uint32_t)shadow.count;
                nvlog_record_t rec = {0};
                nvlog_iter_t it;
                uint32_t n = 0;
                CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
                while (n <= idx) {
                    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
                    n++;
                }
                uint8_t buf[MAX_PAYLOAD];
                CHECK(nvlog_read_payload(&ctx, &rec, buf, rec.len) == NVLOG_OK);
                CHECK(memcmp(buf, shadow.recs[idx].payload, rec.len) == 0);
            }
        } else if (op == 4u) {
            history_add(&hist, "%u: stats", i);
            compare_stats(&hist, &ctx, &shadow);
        } else if (op == 5u) {
            history_add(&hist, "%u: reboot", i);
            nvlog_status_t st = nvlog_ring_mount(&ctx, &hal, MODEL_SIZE);
            CHECK((st == NVLOG_OK && !shadow.corrupted) || (st != NVLOG_OK && shadow.corrupted));
            if (st == NVLOG_OK)
                compare_state(&hist, &ctx, &shadow);
        } else if (op == 6u) {
            history_add(&hist, "%u: corrupt", i);
            if (shadow.count > 0) {
                uint32_t idx = rng_step(&rng) % (uint32_t)shadow.count;
                uint32_t off = shadow.recs[idx].offset + NVLOG_RECORD_HEADER_SIZE + (shadow.recs[idx].len ? (rng_step(&rng) % shadow.recs[idx].len) : 0u);
                if (off < MODEL_SIZE) {
                    shadow.bytes[off] ^= 0x5Au;
                    pctx.ram[off] ^= 0x5Au;
                    shadow.corrupted = 1;
                    CHECK(nvlog_ring_mount(&ctx, &hal, MODEL_SIZE) == NVLOG_ERR_CORRUPT);
                    shadow_format(&shadow);
                    CHECK(nvlog_ring_format(&ctx, &hal, MODEL_SIZE) == NVLOG_OK);
                }
            }
        } else if (op == 7u) {
            history_add(&hist, "%u: seqwrap", i);
            shadow_format(&shadow);
            CHECK(nvlog_ring_format(&ctx, &hal, MODEL_SIZE) == NVLOG_OK);
            shadow.next_seq = UINT32_MAX - 2u;
            ctx.next_seq = UINT32_MAX - 2u;
            for (uint32_t j = 0; j < 5u; j++) {
                uint8_t p[4] = { (uint8_t)j, (uint8_t)(j + 1u), (uint8_t)(j + 2u), (uint8_t)(j + 3u) };
                CHECK(nvlog_append(&ctx, p, sizeof(p)) == NVLOG_OK);
                shadow_append(&shadow, p, sizeof(p));
            }
            CHECK(nvlog_ring_mount(&ctx, &hal, MODEL_SIZE) == NVLOG_OK);
            compare_state(&hist, &ctx, &shadow);
        }
    }

    nvlog_posix_close(&pctx);
}

int main(void)
{
    const uint32_t seeds[SEED_COUNT] = { 1u, 0x12345678u, 0xDEADBEEFu };

    for (uint32_t i = 0; i < SEED_COUNT; i++)
        run_seed(seeds[i]);

    printf("operations=%u scenarios=%u checks=%d\n", SEED_COUNT * OPS_PER_SEED, g_scenarios, g_pass + g_fail);
    printf("PASSED: %d\nFAILED: %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
