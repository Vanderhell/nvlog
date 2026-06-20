/**
 * tests/test_powerloss.c — extended power-loss tests
 *
 * Strategy: inject power-loss at every possible write call
 * within nvlog_append(), verify that after nvlog_mount():
 *   - log is consistent (no corrupt records visible)
 *   - previously committed records are intact
 *   - interrupted record is NOT visible
 *
 * nvlog_append() issues these writes in order:
 *   1. header  (MAGIC+FLAGS+LEN+SEQ = 8 bytes)
 *   2. payload (N bytes, may be split into multiple HAL calls if large)
 *   3. CRC32   (4 bytes)
 *   4. COMMIT  (1 byte) ← commit point
 *
 * Failing before write 4 = record invisible after recovery.
 * Failing during write 3 = CRC partially written = bad CRC = invisible.
 * Failing during write 4 = CRC written but commit missing = invisible.
 *
 * Tests:
 *   PL-01  fail before header        → 0 records after mount
 *   PL-02  fail after header         → 0 records after mount
 *   PL-03  fail after payload        → 0 records after mount (no CRC)
 *   PL-04  success                   → 1 record after mount
 *   PL-05  2 commits + fail on 3rd   → exactly 2 records after mount
 *   PL-06  fail mid-payload (large)  → 0 new records, prior intact
 *   PL-07  repeated append after recovery keeps SEQ monotonic
 *   PL-08  full scan: fail at every write index 0..N for small record
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while(0)

#define TEST(name) fprintf(stdout, "\n[PL] %s\n", name)

#define NVM_SIZE 2048u

/* ─── helpers ─────────────────────────────────────────────────── */

static void fresh_ram(nvlog_posix_ctx_t *pctx, nvlog_hal_t *hal)
{
    nvlog_posix_open_ram(pctx, hal, NVM_SIZE);
}

static uint32_t count_records(nvlog_ctx_t *ctx)
{
    nvlog_iter_t   it;
    nvlog_record_t rec;
    nvlog_iter_init(&it, ctx);
    uint32_t n = 0;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) n++;
    return n;
}

/* mount and count; returns -1 on mount error */
static int mount_and_count(nvlog_hal_t *hal)
{
    nvlog_ctx_t ctx;
    nvlog_ctx_init(&ctx);
    if (nvlog_mount(&ctx, hal, NVM_SIZE) != NVLOG_OK) return -1;
    return (int)count_records(&ctx);
}

/* ─── PL-01: fail before any write ───────────────────────────── */

static void test_pl01(void)
{
    TEST("PL-01: fail before header → 0 records");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);

    nvlog_posix_inject_fail_after(&pctx, 0);  /* fail immediately */
    nvlog_append(&ctx, "data", 4);            /* will fail */

    nvlog_posix_inject_fail_after(&pctx, -1);
    CHECK(mount_and_count(&hal) == 0);

    nvlog_posix_close(&pctx);
}

/* ─── PL-02: fail after header, before payload ────────────────── */

static void test_pl02(void)
{
    TEST("PL-02: fail after header (before payload) → 0 records");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);

    /* write 0 = region header (in format, already done)
       write 1 = record header in append */
    nvlog_posix_inject_fail_after(&pctx, 1);
    nvlog_append(&ctx, "payload", 7);

    nvlog_posix_inject_fail_after(&pctx, -1);
    CHECK(mount_and_count(&hal) == 0);

    nvlog_posix_close(&pctx);
}

/* ─── PL-03: fail after payload, before CRC (commit) ─────────── */

static void test_pl03(void)
{
    TEST("PL-03: fail after payload (before CRC commit) → 0 records");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);

    /* write 1 = record header, write 2 = payload, write 3 = CRC (fails) */
    nvlog_posix_inject_fail_after(&pctx, 2);
    nvlog_append(&ctx, "payload", 7);

    nvlog_posix_inject_fail_after(&pctx, -1);
    CHECK(mount_and_count(&hal) == 0);

    nvlog_posix_close(&pctx);
}

/* ─── PL-04: successful write visible after mount ─────────────── */

static void test_pl04(void)
{
    TEST("PL-04: successful write visible after mount");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);

    CHECK(nvlog_append(&ctx, "ok", 2) == NVLOG_OK);

    CHECK(mount_and_count(&hal) == 1);

    nvlog_posix_close(&pctx);
}

/* ─── PL-05: 2 commits + fail on 3rd → exactly 2 visible ─────── */

static void test_pl05(void)
{
    TEST("PL-05: 2 good + 1 interrupted → 2 records after mount");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);

    CHECK(nvlog_append(&ctx, "first",  5) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "second", 6) == NVLOG_OK);

    /* 3rd append: fail after header only */
    nvlog_posix_inject_fail_after(&pctx, 1);
    nvlog_append(&ctx, "third", 5);

    nvlog_posix_inject_fail_after(&pctx, -1);
    int n = mount_and_count(&hal);
    CHECK(n == 2);

    /* verify content */
    nvlog_ctx_t ctx2;
    nvlog_ctx_init(&ctx2);
    nvlog_mount(&ctx2, &hal, NVM_SIZE);
    nvlog_iter_t it; nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx2);

    char buf[16];
    nvlog_iter_next(&it, &rec);
    memset(buf, 0, sizeof(buf));
    nvlog_read_payload(&ctx2, &rec, buf, sizeof(buf));
    CHECK(memcmp(buf, "first", 5) == 0);

    nvlog_iter_next(&it, &rec);
    memset(buf, 0, sizeof(buf));
    nvlog_read_payload(&ctx2, &rec, buf, sizeof(buf));
    CHECK(memcmp(buf, "second", 6) == 0);

    nvlog_posix_close(&pctx);
}

/* ─── PL-06: large payload, fail mid-payload ──────────────────── */

static void test_pl06(void)
{
    TEST("PL-06: large payload, fail mid-payload → prior records intact");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);

    nvlog_append(&ctx, "anchor", 6);

    /* large payload: 200 bytes — POSIX backend writes it in one call
       but we still fail after write #2 (header written, payload fails) */
    uint8_t big[200];
    memset(big, 0xAB, sizeof(big));

    nvlog_posix_inject_fail_after(&pctx, 1); /* header ok, payload fails */
    nvlog_append(&ctx, big, sizeof(big));

    nvlog_posix_inject_fail_after(&pctx, -1);
    nvlog_ctx_t ctx2;
    nvlog_ctx_init(&ctx2);
    nvlog_mount(&ctx2, &hal, NVM_SIZE);

    CHECK(count_records(&ctx2) == 1);

    nvlog_iter_t it; nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx2);
    nvlog_iter_next(&it, &rec);
    CHECK(rec.seq == 0);
    CHECK(rec.len == 6);

    nvlog_posix_close(&pctx);
}

/* ─── PL-07: SEQ monotonic after recovery + new appends ──────── */

static void test_pl07(void)
{
    TEST("PL-07: SEQ stays monotonic after recovery + new appends");
    nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);

    /* session 1 */
    nvlog_ctx_t ctx1;
    nvlog_format(&ctx1, &hal, NVM_SIZE);
    nvlog_append(&ctx1, "a", 1);
    nvlog_append(&ctx1, "b", 1);

    /* simulate crash mid-3rd append */
    nvlog_posix_inject_fail_after(&pctx, 1);
    nvlog_append(&ctx1, "c", 1);
    nvlog_posix_inject_fail_after(&pctx, -1);

    /* session 2: mount + append */
    nvlog_ctx_t ctx2;
    nvlog_ctx_init(&ctx2);
    nvlog_mount(&ctx2, &hal, NVM_SIZE);
    CHECK(ctx2.next_seq == 2);  /* seq 0 and 1 committed, 2 was interrupted */

    nvlog_append(&ctx2, "d", 1);
    nvlog_append(&ctx2, "e", 1);

    /* verify all 4 records have monotonic SEQ */
    nvlog_iter_t it; nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx2);

    uint32_t prev_seq = UINT32_MAX;
    uint32_t count = 0;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
        if (prev_seq != UINT32_MAX)
            CHECK(rec.seq == prev_seq + 1);
        prev_seq = rec.seq;
        count++;
    }
    CHECK(count == 4);
    CHECK(rec.seq == 3);

    nvlog_posix_close(&pctx);
}

/* ─── PL-08: exhaustive — fail at every write index ──────────── */

static void test_pl08(void)
{
    TEST("PL-08: exhaustive fail-at-every-write-index");

    /*
     * For a single-append sequence, writes issued are:
     *   index 0: commit placeholder byte (cleared to 0x00)
     *   index 1: record header
     *   index 2: record payload
     *   index 3: CRC
     *   index 4: commit byte
     *
     * Failing at index 0..4 → 0 records after mount
     * No failure (index=99) → 1 record
     */

    /*
     * nvlog_posix_inject_fail_after(N): allows N writes, fails on write N.
     *
     * nvlog_append("test",4) issues exactly 5 writes after format:
     *   write 0: commit placeholder (1B)
     *   write 1: record header      (8B)
     *   write 2: record payload     (4B)
     *   write 3: CRC32             (4B)
     *   write 4: COMMIT             (1B) — commit point
     *
     * fail_after=0 → placeholder fails → 0 records after mount
     * fail_after=1 → header fails      → 0 records after mount
     * fail_after=2 → payload fails     → 0 records after mount
     * fail_after=3 → CRC fails         → 0 records after mount
     * fail_after=4 → commit fails       → 0 records after mount
     * fail_after=5 → all succeed        → 1 record  after mount
     */
    for (int32_t fail_after = 0; fail_after <= 5; fail_after++) {
        nvlog_posix_ctx_t pctx; nvlog_hal_t hal; fresh_ram(&pctx, &hal);
        nvlog_ctx_t ctx;
        nvlog_format(&ctx, &hal, NVM_SIZE);

        nvlog_posix_inject_fail_after(&pctx, fail_after);
        nvlog_append(&ctx, "test", 4);
        nvlog_posix_inject_fail_after(&pctx, -1);

        int n = mount_and_count(&hal);
        if (fail_after < 5) {
            CHECK(n == 0);   /* any pre-commit failure → invisible */
        } else {
            CHECK(n == 1);   /* all writes ok → visible */
        }

        nvlog_posix_close(&pctx);
    }
}

/* ─── main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("nvlog power-loss test suite\n");
    printf("====================================\n");

    test_pl01();
    test_pl02();
    test_pl03();
    test_pl04();
    test_pl05();
    test_pl06();
    test_pl07();
    test_pl08();

    printf("\n====================================\n");
    printf("PASSED: %d\n", g_pass);
    printf("FAILED: %d\n", g_fail);
    printf("====================================\n");

    return g_fail == 0 ? 0 : 1;
}
