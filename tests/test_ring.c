/**
 * tests/test_ring.c — Ring mode test suite
 *
 * Tests:
 *   RG-01  basic ring: append + iter (no wrap yet)
 *   RG-02  ring wraps: oldest records overwritten, newest visible
 *   RG-03  iter order: oldest-first across wrap boundary
 *   RG-04  SEQ is monotonically increasing across wrap
 *   RG-05  power-loss during ring append → interrupted record invisible
 *   RG-06  ring_mount recovery: empty ring
 *   RG-07  ring_mount recovery: ring with records, no wrap
 *   RG-08  ring_mount recovery: ring has wrapped, correct tail restored
 *   RG-09  ring_mount recovery: power-loss at CRC → tail/write_ptr correct
 *   RG-10  ring_count() returns correct count before and after wrap
 *   RG-11  record too large for ring → NVLOG_ERR_FULL
 *   RG-12  linear API still works (backward compat)
 *   RG-13  fill ring exactly, then one more write evicts oldest
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

#define TEST(name) fprintf(stdout, "\n[RG] %s\n", name)

/*
 * Small region: 8 (region header) + 5 records × (12 overhead + 4 payload) = 8 + 80 = 88 bytes.
 * We'll use RING_SIZE = 88 → fits exactly 5 records, then wraps.
 */
#define PAYLOAD_LEN  4u
#define RECORD_SIZE  (NVLOG_RECORD_OVERHEAD + PAYLOAD_LEN)   /* 16 */
#define NUM_RECORDS  5u
#define RING_SIZE    ((uint32_t)(NVLOG_REGION_HEADER_SIZE + NUM_RECORDS * RECORD_SIZE))  /* 88 */

/* ─── helpers ─────────────────────────────────────────────────── */

static void fresh(nvlog_posix_ctx_t *p, nvlog_hal_t *h)
{
    nvlog_posix_open_ram(p, h, RING_SIZE);
}

static uint32_t count(nvlog_ctx_t *ctx)
{
    return nvlog_ring_count(ctx);
}

/* Collect all records into seq[] array, return count */
static uint32_t collect_seqs(nvlog_ctx_t *ctx, uint32_t *seq_buf, uint32_t buf_len)
{
    nvlog_iter_t it; nvlog_record_t rec;
    nvlog_iter_init(&it, ctx);
    uint32_t n = 0;
    while (n < buf_len && nvlog_iter_next(&it, &rec) == NVLOG_OK)
        seq_buf[n++] = rec.seq;
    return n;
}

/* Write 'n' records with payload = 4-byte little-endian index */
static void write_n(nvlog_ctx_t *ctx, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = ctx->next_seq;   /* use seq as payload too */
        nvlog_status_t st = nvlog_append(ctx, &v, sizeof(v));
        (void)st;
    }
}

/* ─── RG-01: basic, no wrap ───────────────────────────────────── */

static void test_rg01(void)
{
    TEST("RG-01: basic append + iter (no wrap)");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    CHECK(nvlog_ring_format(&ctx, &h, RING_SIZE) == NVLOG_OK);
    CHECK(ctx.mode == NVLOG_MODE_RING);

    write_n(&ctx, 3);
    CHECK(count(&ctx) == 3);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx, seqs, 8);
    CHECK(n == 3);
    CHECK(seqs[0] == 0 && seqs[1] == 1 && seqs[2] == 2);

    nvlog_posix_close(&p);
}

/* ─── RG-02: wrap — oldest overwritten ───────────────────────── */

static void test_rg02(void)
{
    TEST("RG-02: wrap — oldest records overwritten");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    /* fill completely (5 records) */
    write_n(&ctx, NUM_RECORDS);
    CHECK(count(&ctx) == NUM_RECORDS);

    /* write one more — should evict record 0, ring holds 1..5 */
    write_n(&ctx, 1);
    CHECK(count(&ctx) == NUM_RECORDS);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx, seqs, 8);
    CHECK(n == NUM_RECORDS);
    CHECK(seqs[0] == 1);                       /* oldest = seq 1 */
    CHECK(seqs[n-1] == NUM_RECORDS);           /* newest = seq 5 */

    nvlog_posix_close(&p);
}

/* ─── RG-03: iter order across wrap boundary ──────────────────── */

static void test_rg03(void)
{
    TEST("RG-03: iter yields oldest-first across wrap");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    /* write 7 records into a 5-slot ring → evicts 0,1 */
    write_n(&ctx, NUM_RECORDS + 2);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx, seqs, 8);
    CHECK(n == NUM_RECORDS);
    /* should be 2,3,4,5,6 in order */
    for (uint32_t i = 0; i < n; i++)
        CHECK(seqs[i] == 2 + i);

    nvlog_posix_close(&p);
}

/* ─── RG-04: SEQ monotonically increasing across wrap ─────────── */

static void test_rg04(void)
{
    TEST("RG-04: SEQ monotonically increasing across wraps");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    /* write 3× capacity */
    write_n(&ctx, NUM_RECORDS * 3);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx, seqs, 8);
    CHECK(n == NUM_RECORDS);

    for (uint32_t i = 1; i < n; i++)
        CHECK(seqs[i] == seqs[i-1] + 1);   /* strictly increasing */

    nvlog_posix_close(&p);
}

/* ─── RG-05: power-loss during ring append ────────────────────── */

static void test_rg05(void)
{
    TEST("RG-05: power-loss during ring append → record invisible");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    write_n(&ctx, 2);  /* seq 0, 1 */

    /* fail after header write — CRC never written */
    nvlog_posix_inject_fail_after(&p, 1);
    uint32_t v = 99;
    nvlog_append(&ctx, &v, sizeof(v));
    nvlog_posix_inject_fail_after(&p, -1);

    /* mount and verify: only seq 0 and 1 visible */
    nvlog_ctx_t ctx2;
    CHECK(nvlog_ring_mount(&ctx2, &h, RING_SIZE) == NVLOG_OK);
    CHECK(count(&ctx2) == 2);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx2, seqs, 8);
    CHECK(n == 2);
    CHECK(seqs[0] == 0 && seqs[1] == 1);

    nvlog_posix_close(&p);
}

/* ─── RG-06: mount empty ring ─────────────────────────────────── */

static void test_rg06(void)
{
    TEST("RG-06: ring_mount on empty ring");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);

    nvlog_ctx_t ctx1;
    nvlog_ring_format(&ctx1, &h, RING_SIZE);
    /* no appends */

    nvlog_ctx_t ctx2;
    CHECK(nvlog_ring_mount(&ctx2, &h, RING_SIZE) == NVLOG_OK);
    CHECK(count(&ctx2) == 0);
    CHECK(ctx2.next_seq == 0);

    /* can still append after mount */
    uint32_t v = 42;
    CHECK(nvlog_append(&ctx2, &v, sizeof(v)) == NVLOG_OK);
    CHECK(count(&ctx2) == 1);

    nvlog_posix_close(&p);
}

/* ─── RG-07: mount ring with records, no wrap ─────────────────── */

static void test_rg07(void)
{
    TEST("RG-07: ring_mount with records, no wrap yet");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);

    nvlog_ctx_t ctx1;
    nvlog_ring_format(&ctx1, &h, RING_SIZE);
    write_n(&ctx1, 3);

    nvlog_ctx_t ctx2;
    CHECK(nvlog_ring_mount(&ctx2, &h, RING_SIZE) == NVLOG_OK);
    CHECK(count(&ctx2) == 3);
    CHECK(ctx2.next_seq == 3);

    /* append more after recovery */
    write_n(&ctx2, 2);
    CHECK(count(&ctx2) == 5);

    nvlog_posix_close(&p);
}

/* ─── RG-08: mount after wrap — tail restored correctly ───────── */

static void test_rg08(void)
{
    TEST("RG-08: ring_mount after wrap — tail_ptr restored correctly");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);

    nvlog_ctx_t ctx1;
    nvlog_ring_format(&ctx1, &h, RING_SIZE);
    /* write 7 records → evicts 0,1 */
    write_n(&ctx1, NUM_RECORDS + 2);

    nvlog_ctx_t ctx2;
    CHECK(nvlog_ring_mount(&ctx2, &h, RING_SIZE) == NVLOG_OK);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx2, seqs, 8);
    CHECK(n == NUM_RECORDS);
    CHECK(seqs[0] == 2);        /* oldest after eviction */
    CHECK(seqs[n-1] == 6);      /* newest */
    CHECK(ctx2.next_seq == 7);

    nvlog_posix_close(&p);
}

/* ─── RG-09: mount after power-loss at CRC ────────────────────── */

static void test_rg09(void)
{
    TEST("RG-09: ring_mount after power-loss (CRC not written)");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);

    nvlog_ctx_t ctx1;
    nvlog_ring_format(&ctx1, &h, RING_SIZE);
    write_n(&ctx1, 3);   /* seq 0,1,2 committed */

    /* fail before CRC of record seq=3 */
    nvlog_posix_inject_fail_after(&p, 2);  /* header+payload ok, CRC fails */
    write_n(&ctx1, 1);
    nvlog_posix_inject_fail_after(&p, -1);

    nvlog_ctx_t ctx2;
    CHECK(nvlog_ring_mount(&ctx2, &h, RING_SIZE) == NVLOG_OK);
    CHECK(count(&ctx2) == 3);
    CHECK(ctx2.next_seq == 3);   /* seq 3 was not committed */

    /* verify correct records visible */
    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx2, seqs, 8);
    CHECK(n == 3);
    CHECK(seqs[0] == 0 && seqs[1] == 1 && seqs[2] == 2);

    nvlog_posix_close(&p);
}

/* ─── RG-10: ring_count ───────────────────────────────────────── */

static void test_rg10(void)
{
    TEST("RG-10: ring_count before and after wrap");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    for (uint32_t i = 0; i < NUM_RECORDS * 3; i++) {
        write_n(&ctx, 1);
        uint32_t c = count(&ctx);
        uint32_t expected = (i + 1) < NUM_RECORDS ? (i + 1) : NUM_RECORDS;
        CHECK(c == expected);
    }

    nvlog_posix_close(&p);
}

/* ─── RG-11: record too large for ring ────────────────────────── */

static void test_rg11(void)
{
    TEST("RG-11: record larger than ring capacity → NVLOG_ERR_FULL");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    /* payload that exceeds entire ring data area */
    uint32_t cap = RING_SIZE - (uint32_t)NVLOG_REGION_HEADER_SIZE;
    uint8_t  *big = (uint8_t *)malloc(cap);
    memset(big, 0xCC, cap);

    nvlog_status_t st = nvlog_append(&ctx, big, (uint16_t)(cap - 1));
    CHECK(st == NVLOG_ERR_FULL);

    free(big);
    nvlog_posix_close(&p);
}

/* ─── RG-12: linear API unaffected (backward compat) ─────────── */

static void test_rg12(void)
{
    TEST("RG-12: linear mode still works (backward compat)");
    nvlog_posix_ctx_t p; nvlog_hal_t h;
    nvlog_posix_open_ram(&p, &h, RING_SIZE);

    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &h, RING_SIZE);
    CHECK(ctx.mode == NVLOG_MODE_LINEAR);

    write_n(&ctx, 3);

    nvlog_ctx_t ctx2;
    CHECK(nvlog_mount(&ctx2, &h, RING_SIZE) == NVLOG_OK);
    CHECK(ctx2.mode == NVLOG_MODE_LINEAR);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx2, seqs, 8);
    CHECK(n == 3);
    CHECK(seqs[0] == 0 && seqs[1] == 1 && seqs[2] == 2);

    nvlog_posix_close(&p);
}

/* ─── RG-13: fill exactly, then evict oldest ──────────────────── */

static void test_rg13(void)
{
    TEST("RG-13: fill ring exactly, one more write evicts oldest");
    nvlog_posix_ctx_t p; nvlog_hal_t h; fresh(&p, &h);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, RING_SIZE);

    write_n(&ctx, NUM_RECORDS);     /* fill exactly: seq 0..4 */
    CHECK(count(&ctx) == NUM_RECORDS);

    /* write seq=5: ring must now hold seq 1..5 */
    write_n(&ctx, 1);
    CHECK(count(&ctx) == NUM_RECORDS);

    uint32_t seqs[8];
    uint32_t n = collect_seqs(&ctx, seqs, 8);
    CHECK(n == NUM_RECORDS);
    CHECK(seqs[0] == 1);            /* seq 0 evicted */
    CHECK(seqs[n-1] == 5);

    /* read payload of first record and verify it's seq=1 */
    nvlog_iter_t it; nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx);
    nvlog_iter_next(&it, &rec);
    uint32_t payload = 0;
    nvlog_read_payload(&ctx, &rec, &payload, sizeof(payload));
    CHECK(payload == 1);            /* payload == seq number for write_n() */

    nvlog_posix_close(&p);
}

static void test_rg14(void)
{
    TEST("RG-14: record_count correct after multi-record eviction");
    /* Use a region that fits exactly 3 small records */
    uint32_t small_ring = (uint32_t)(NVLOG_REGION_HEADER_SIZE + 3 * RECORD_SIZE);
    nvlog_posix_ctx_t p; nvlog_hal_t h;
    nvlog_posix_open_ram(&p, &h, small_ring);
    nvlog_ctx_t ctx;
    nvlog_ring_format(&ctx, &h, small_ring);

    /* fill: 3 records */
    write_n(&ctx, 3);
    CHECK(nvlog_ring_count(&ctx) == 3);

    /* one more: evicts 1, count stays 3 */
    write_n(&ctx, 1);
    CHECK(nvlog_ring_count(&ctx) == 3);

    /* five more: count must never exceed 3 */
    write_n(&ctx, 5);
    CHECK(nvlog_ring_count(&ctx) == 3);

    /* mount and verify count matches reality */
    nvlog_ctx_t ctx2;
    nvlog_ring_mount(&ctx2, &h, small_ring);
    CHECK(nvlog_ring_count(&ctx2) == 3);

    nvlog_posix_close(&p);
}

/* ─── main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("nvlog ring mode test suite\n");
    printf("===================================\n");

    test_rg01();
    test_rg02();
    test_rg03();
    test_rg04();
    test_rg05();
    test_rg06();
    test_rg07();
    test_rg08();
    test_rg09();
    test_rg10();
    test_rg11();
    test_rg12();
    test_rg13();
    test_rg14();

    printf("\n===================================\n");
    printf("PASSED: %d\n", g_pass);
    printf("FAILED: %d\n", g_fail);
    printf("===================================\n");

    return g_fail == 0 ? 0 : 1;
}
