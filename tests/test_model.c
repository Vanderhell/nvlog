#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

typedef struct {
    uint32_t seq;
    uint16_t len;
    uint8_t *data;
} model_rec_t;

typedef struct {
    model_rec_t *recs;
    size_t count;
    size_t cap;
} model_vec_t;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

static void vec_free(model_vec_t *v)
{
    if (!v) return;
    for (size_t i = 0; i < v->count; i++) free(v->recs[i].data);
    free(v->recs);
    memset(v, 0, sizeof(*v));
}

static int vec_push(model_vec_t *v, uint32_t seq, const void *data, uint16_t len)
{
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 16;
        void *np = realloc(v->recs, ncap * sizeof(*v->recs));
        if (!np) return -1;
        v->recs = (model_rec_t *)np;
        v->cap = ncap;
    }
    v->recs[v->count].seq = seq;
    v->recs[v->count].len = len;
    v->recs[v->count].data = (uint8_t *)malloc(len ? len : 1);
    if (len && !v->recs[v->count].data) return -1;
    if (len) memcpy(v->recs[v->count].data, data, len);
    v->count++;
    return 0;
}

static void vec_pop_front(model_vec_t *v)
{
    if (v->count == 0) return;
    free(v->recs[0].data);
    memmove(v->recs, v->recs + 1, (v->count - 1) * sizeof(*v->recs));
    v->count--;
}

static uint32_t vec_used_bytes(const model_vec_t *v)
{
    uint32_t used = 0;
    for (size_t i = 0; i < v->count; i++)
        used += NVLOG_RECORD_OVERHEAD + v->recs[i].len;
    return used;
}

static int vec_equal(const model_vec_t *a, const model_vec_t *b)
{
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) {
        if (a->recs[i].seq != b->recs[i].seq ||
            a->recs[i].len != b->recs[i].len)
            return 0;
        if (a->recs[i].len &&
            memcmp(a->recs[i].data, b->recs[i].data, a->recs[i].len) != 0)
            return 0;
    }
    return 1;
}

static void collect_iter(nvlog_ctx_t *ctx, model_vec_t *out)
{
    nvlog_iter_t it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, ctx) == NVLOG_OK);
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
        uint8_t tmp[256];
        CHECK(rec.len <= sizeof(tmp));
        CHECK(nvlog_read_payload(ctx, &rec, tmp, sizeof(tmp)) == NVLOG_OK);
        CHECK(vec_push(out, rec.seq, tmp, rec.len) == 0);
    }
}

int main(void)
{
    const uint32_t seed = 0xC0FFEEu;
    srand(seed);
    printf("nvlog model test seed=%u\n", seed);

    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    CHECK(nvlog_posix_open_ram(&pctx, &hal, 4096u) == 0);

    nvlog_ctx_t ctx;
    model_vec_t model = {0};
    uint8_t payload[48];
    uint32_t next_seq = 0;
    int ring_mode = 0;
    int mounted = 0;

    for (int i = 0; i < 5000; i++) {
        uint32_t op = (uint32_t)(rand() % 8);
        uint16_t len = (uint16_t)(rand() % sizeof(payload));
        for (uint16_t j = 0; j < len; j++) payload[j] = (uint8_t)(rand() & 0xFF);

        if (op == 0) {
            CHECK((ring_mode ? nvlog_ring_format(&ctx, &hal, 4096u)
                             : nvlog_format(&ctx, &hal, 4096u)) == NVLOG_OK);
            vec_free(&model);
            next_seq = 0;
            mounted = 1;
        } else if (op == 1) {
            ring_mode = !ring_mode;
            CHECK((ring_mode ? nvlog_ring_format(&ctx, &hal, 4096u)
                             : nvlog_format(&ctx, &hal, 4096u)) == NVLOG_OK);
            vec_free(&model);
            next_seq = 0;
            mounted = 1;
        } else if (op == 2 && mounted) {
            int fail_after = (rand() % 6 == 0) ? (rand() % 3) : -1;
            nvlog_posix_inject_fail_after(&pctx, fail_after);
            nvlog_status_t st = nvlog_append(&ctx, payload, len);
            nvlog_posix_inject_fail_after(&pctx, -1);
            if (st == NVLOG_OK) {
                if (!ring_mode) {
                    CHECK(vec_push(&model, next_seq++, payload, len) == 0);
                } else {
                    while (vec_used_bytes(&model) + NVLOG_RECORD_OVERHEAD + len >
                           (4096u - (uint32_t)NVLOG_REGION_HEADER_SIZE) && model.count > 0)
                        vec_pop_front(&model);
                    CHECK(vec_push(&model, next_seq++, payload, len) == 0);
                }
            } else {
                nvlog_ctx_t rm;
                CHECK((ring_mode ? nvlog_ring_mount(&rm, &hal, 4096u)
                                 : nvlog_mount(&rm, &hal, 4096u)) == NVLOG_OK);
                mounted = 1;
                ctx = rm;
                model_vec_t actual = {0};
                collect_iter(&ctx, &actual);
                vec_free(&model);
                model = actual;
            }
        } else if (op == 3 && mounted) {
            nvlog_ctx_t rm;
            CHECK((ring_mode ? nvlog_ring_mount(&rm, &hal, 4096u)
                             : nvlog_mount(&rm, &hal, 4096u)) == NVLOG_OK);
            ctx = rm;
            model_vec_t actual = {0};
            collect_iter(&ctx, &actual);
            CHECK(vec_equal(&actual, &model));
            vec_free(&actual);
        } else if (op == 4 && pctx.ram) {
            size_t off = (size_t)(rand() % 4096u);
            pctx.ram[off] ^= 0x5Au;
            nvlog_ctx_t rm;
            CHECK((ring_mode ? nvlog_ring_mount(&rm, &hal, 4096u)
                             : nvlog_mount(&rm, &hal, 4096u)) == NVLOG_OK);
            mounted = 1;
            ctx = rm;
            model_vec_t actual = {0};
            collect_iter(&ctx, &actual);
            vec_free(&model);
            model = actual;
        } else if (op == 5 && mounted) {
            nvlog_posix_inject_fail_after(&pctx, 1);
            nvlog_status_t st = nvlog_append(&ctx, payload, len);
            nvlog_posix_inject_fail_after(&pctx, -1);
            CHECK(st == NVLOG_ERR_IO || st == NVLOG_OK);
            nvlog_ctx_t rm;
            CHECK((ring_mode ? nvlog_ring_mount(&rm, &hal, 4096u)
                             : nvlog_mount(&rm, &hal, 4096u)) == NVLOG_OK);
            ctx = rm;
            model_vec_t actual = {0};
            collect_iter(&ctx, &actual);
            vec_free(&model);
            model = actual;
        } else if (op == 6 && mounted) {
            nvlog_stats_t s;
            CHECK(nvlog_stats(&ctx, &s) == NVLOG_OK);
        } else if (op == 7 && mounted && model.count > 0) {
            nvlog_iter_t it;
            nvlog_record_t rec;
            CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
            CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
        }
    }

    vec_free(&model);
    nvlog_posix_close(&pctx);
    printf("PASSED: %d\nFAILED: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
