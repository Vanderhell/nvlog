#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

#define TEST(name) do { \
    printf("\n[%s]\n", name); \
} while (0)

static int g_pass = 0;
static int g_fail = 0;

static uint32_t ring_size(uint32_t slack)
{
    return (uint32_t)(NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_OVERHEAD + slack);
}

static uint32_t ring_large_size(void)
{
    return ring_size(NVLOG_MAX_PAYLOAD + 4096u);
}

static void open_ring(nvlog_posix_ctx_t *pctx, nvlog_hal_t *hal, uint32_t size)
{
    CHECK(nvlog_posix_open_ram(pctx, hal, size) == 0);
}

static void close_ring(nvlog_posix_ctx_t *pctx)
{
    nvlog_posix_close(pctx);
}

static void make_payload(uint8_t *buf, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i);
}

static void append_record(nvlog_ctx_t *ctx, uint32_t seq, uint32_t len)
{
    uint8_t payload[65536];
    if (len > sizeof(payload)) len = (uint32_t)sizeof(payload);
    make_payload(payload, len, seq);
    nvlog_status_t st = nvlog_append(ctx, payload, (uint16_t)len);
    CHECK(st == NVLOG_OK);
}

static uint32_t collect_records(nvlog_ctx_t *ctx, nvlog_record_t *recs, uint32_t max)
{
    nvlog_iter_t it;
    uint32_t n = 0;
    CHECK(nvlog_iter_init(&it, ctx) == NVLOG_OK);
    while (n < max) {
        nvlog_status_t st = nvlog_iter_next(&it, &recs[n]);
        if (st == NVLOG_ERR_NO_DATA)
            break;
        CHECK(st == NVLOG_OK);
        n++;
    }
    return n;
}

static void assert_stats(nvlog_ctx_t *ctx, uint32_t used, uint32_t freeb, uint32_t count, uint32_t next_seq)
{
    nvlog_stats_t st;
    CHECK(nvlog_stats(ctx, &st) == NVLOG_OK);
    CHECK(st.used_bytes == used);
    CHECK(st.free_bytes == freeb);
    CHECK(st.record_count == count);
    CHECK(st.next_seq == next_seq);
}

static void test_empty_ring(void)
{
    TEST("ring empty");
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, ring_size(1024u));

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, ring_size(1024u)) == NVLOG_OK);
    assert_stats(&ctx, 0, ctx.free_bytes, 0, 0);

    nvlog_iter_t it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_NO_DATA);

    close_ring(&pctx);
}

static void test_one_record(void)
{
    TEST("ring one record");
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, ring_size(2048u));

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, ring_size(2048u)) == NVLOG_OK);
    append_record(&ctx, 0, 4u);

    nvlog_record_t recs[4];
    uint32_t n = collect_records(&ctx, recs, 4);
    CHECK(n == 1);
    CHECK(recs[0].seq == 0);
    CHECK(recs[0].len == 4);

    uint8_t payload[4];
    CHECK(nvlog_read_payload(&ctx, &recs[0], payload, sizeof(payload)) == NVLOG_OK);
    CHECK(payload[0] == 0 && payload[1] == 1 && payload[2] == 2 && payload[3] == 3);

    close_ring(&pctx);
}

static void test_zero_length(void)
{
    TEST("ring zero length");
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, ring_size(2048u));

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, ring_size(2048u)) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, NULL, 0) == NVLOG_OK);
    assert_stats(&ctx, NVLOG_RECORD_OVERHEAD, ctx.free_bytes, 1, 1);

    nvlog_record_t recs[2];
    uint32_t n = collect_records(&ctx, recs, 2);
    CHECK(n == 1);
    CHECK(recs[0].len == 0);

    close_ring(&pctx);
}

static void test_max_record(void)
{
    TEST("ring maximum record");
    uint32_t size = ring_size(NVLOG_MAX_PAYLOAD + 128u);
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    uint8_t payload[NVLOG_MAX_PAYLOAD];
    memset(payload, 0xA5, sizeof(payload));
    CHECK(nvlog_append(&ctx, payload, (uint16_t)sizeof(payload)) == NVLOG_OK);
    assert_stats(&ctx, NVLOG_RECORD_OVERHEAD + NVLOG_MAX_PAYLOAD, ctx.free_bytes, 1, 1);

    close_ring(&pctx);
}

static void test_exact_end(void)
{
    TEST("ring exact end");
    uint32_t len = 256u;
    uint32_t size = (uint32_t)(NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_OVERHEAD + len);
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    append_record(&ctx, 7u, len);
    CHECK(ctx.write_ptr == NVLOG_REGION_HEADER_SIZE);

    nvlog_record_t recs[2];
    uint32_t n = collect_records(&ctx, recs, 2);
    CHECK(n == 1);
    CHECK(recs[0].seq == 0 && recs[0].len == len);

    close_ring(&pctx);
}

static void test_mixed_payloads(void)
{
    TEST("ring mixed payloads");
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    uint32_t size = ring_large_size();
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    const uint32_t lengths[] = {0u, 1u, 3u, 7u, 16u, 31u, 4u, 2u};
    for (uint32_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
        append_record(&ctx, i, lengths[i]);

    nvlog_record_t recs[16];
    uint32_t n = collect_records(&ctx, recs, 16);
    CHECK(n == sizeof(lengths) / sizeof(lengths[0]));
    for (uint32_t i = 0; i < n; i++) {
        CHECK(recs[i].seq == i);
        CHECK(recs[i].len == lengths[i]);
    }

    close_ring(&pctx);
}

static void test_repeated_wraps(void)
{
    TEST("ring repeated wraps");
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    uint32_t size = ring_size(4096u);
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    for (uint32_t i = 0; i < 400u; i++)
        append_record(&ctx, i, (uint32_t)(i % 23u));

    nvlog_ctx_t rm;
    CHECK(nvlog_ring_mount(&rm, &hal, size) == NVLOG_OK);
    CHECK(nvlog_ring_count(&rm) > 0);
    CHECK(rm.next_seq == 400u);

    nvlog_record_t recs[512];
    uint32_t n = collect_records(&rm, recs, 512);
    CHECK(n == nvlog_ring_count(&rm));
    for (uint32_t i = 1; i < n; i++)
        CHECK((uint32_t)(recs[i - 1].seq + 1u) == recs[i].seq);

    close_ring(&pctx);
}

static void test_corrupt_header_payload_commit(void)
{
    TEST("ring corrupt header/payload/commit");
    uint32_t size = ring_large_size();
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    append_record(&ctx, 0, 8u);

    pctx.ram[NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_HEADER_SIZE] ^= 0x40u;
    CHECK(nvlog_ring_mount(&ctx, &hal, size) == NVLOG_ERR_CORRUPT);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_HEADER_SIZE] ^= 0x40u;

    pctx.ram[NVLOG_REGION_HEADER_SIZE + 1u] ^= 0x01u;
    CHECK(nvlog_ring_mount(&ctx, &hal, size) == NVLOG_ERR_CORRUPT);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + 1u] ^= 0x01u;

    pctx.ram[NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_HEADER_SIZE + 8u + 4u] ^= 0x01u;
    CHECK(nvlog_ring_mount(&ctx, &hal, size) == NVLOG_ERR_CORRUPT);

    close_ring(&pctx);
}

static void test_interrupted_append(void)
{
    TEST("ring interrupted append");
    uint32_t size = ring_large_size();
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    append_record(&ctx, 0, 4u);

    nvlog_posix_inject_fail_after(&pctx, 1);
    uint8_t payload[4] = {9, 8, 7, 6};
    nvlog_status_t st = nvlog_append(&ctx, payload, 4u);
    CHECK(st == NVLOG_ERR_IO);
    nvlog_posix_inject_fail_after(&pctx, -1);

    nvlog_ctx_t rm;
    CHECK(nvlog_ring_mount(&rm, &hal, size) == NVLOG_OK);
    nvlog_record_t recs[8];
    uint32_t n = collect_records(&rm, recs, 8);
    CHECK(n == 1);
    CHECK(recs[0].seq == 0);

    CHECK(nvlog_append(&rm, payload, 4u) == NVLOG_OK);
    CHECK(nvlog_ring_mount(&ctx, &hal, size) == NVLOG_OK);
    n = collect_records(&ctx, recs, 8);
    CHECK(n == 2);

    close_ring(&pctx);
}

static void test_iterator_invalidation(void)
{
    TEST("ring iterator invalidation");
    uint32_t size = ring_large_size();
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    append_record(&ctx, 0, 4u);

    nvlog_iter_t it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
    append_record(&ctx, 1, 4u);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_STALE);

    close_ring(&pctx);
}

static void test_stale_descriptor(void)
{
    TEST("ring stale descriptor");
    uint32_t size = ring_size(128u);
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    append_record(&ctx, 0, 4u);

    nvlog_record_t recs[2];
    uint32_t n = collect_records(&ctx, recs, 2);
    CHECK(n == 1);
    for (uint32_t i = 1; i < 8u; i++)
        append_record(&ctx, i, 4u);
    uint8_t buf[4];
    CHECK(nvlog_read_payload(&ctx, &recs[0], buf, sizeof(buf)) == NVLOG_ERR_STALE);

    close_ring(&pctx);
}

static void test_sequence_wraparound(void)
{
    TEST("ring sequence wrap");
    uint32_t size = ring_large_size();
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    ctx.next_seq = UINT32_MAX - 2u;
    ctx.metadata_seq = 1u;
    for (uint32_t i = 0; i < 5u; i++)
        CHECK(nvlog_append(&ctx, &i, sizeof(i)) == NVLOG_OK);

    nvlog_ctx_t rm;
    CHECK(nvlog_ring_mount(&rm, &hal, size) == NVLOG_OK);
    nvlog_record_t recs[8];
    uint32_t n = collect_records(&rm, recs, 8);
    CHECK(n == 5u);
    CHECK(recs[0].seq == UINT32_MAX - 2u);
    CHECK(recs[1].seq == UINT32_MAX - 1u);
    CHECK(recs[2].seq == UINT32_MAX);
    CHECK(recs[3].seq == 0u);
    CHECK(recs[4].seq == 1u);

    close_ring(&pctx);
}

static void test_full_capacity(void)
{
    TEST("ring full capacity rejection");
    uint32_t size = ring_size(32u);
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    uint8_t payload[128];
    memset(payload, 0xCC, sizeof(payload));
    nvlog_stats_t st;
    CHECK(nvlog_stats(&ctx, &st) == NVLOG_OK);
    uint32_t before_count = st.record_count;
    uint32_t before_used = st.used_bytes;
    uint32_t before_free = st.free_bytes;
    CHECK(nvlog_append(&ctx, payload, (uint16_t)sizeof(payload)) == NVLOG_ERR_FULL);
    CHECK(nvlog_stats(&ctx, &st) == NVLOG_OK);
    CHECK(st.record_count == before_count);
    CHECK(st.used_bytes == before_used);
    CHECK(st.free_bytes == before_free);

    close_ring(&pctx);
}

static void test_old_or_new_overwrite(void)
{
    TEST("ring old-or-new overwrite");
    uint32_t size = ring_large_size();
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    open_ring(&pctx, &hal, size);

    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &hal, size) == NVLOG_OK);
    append_record(&ctx, 0, 8u);
    append_record(&ctx, 1, 8u);

    nvlog_posix_inject_fail_after(&pctx, 2);
    uint8_t payload[8] = {1,2,3,4,5,6,7,8};
    nvlog_status_t st = nvlog_append(&ctx, payload, 8u);
    nvlog_posix_inject_fail_after(&pctx, -1);
    CHECK(st == NVLOG_ERR_IO || st == NVLOG_OK);

    nvlog_ctx_t rm;
    CHECK(nvlog_ring_mount(&rm, &hal, size) == NVLOG_OK);
    nvlog_record_t recs[8];
    uint32_t n = collect_records(&rm, recs, 8);
    CHECK(n >= 2u && n <= 3u);
    CHECK(recs[0].seq == 0u);

    close_ring(&pctx);
}

int main(void)
{
    printf("nvlog ring tests\n");

    test_empty_ring();
    test_one_record();
    test_zero_length();
    test_max_record();
    test_exact_end();
    test_mixed_payloads();
    test_repeated_wraps();
    test_corrupt_header_payload_commit();
    test_interrupted_append();
    test_iterator_invalidation();
    test_stale_descriptor();
    test_sequence_wraparound();
    test_full_capacity();
    test_old_or_new_overwrite();

    printf("PASSED: %d\nFAILED: %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
