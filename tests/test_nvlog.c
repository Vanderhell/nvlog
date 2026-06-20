/**
 * tests/test_nvlog.c
 *
 * Tests:
 *   1. format + basic append + iter
 *   2. multiple records, correct SEQ order
 *   3. bad CRC detection
 *   4. mount recovery (find last valid record)
 *   5. power-loss during append — partial record invisible after mount
 *   6. buffer full
 *   7. stats
 *   8. zero-length payload
 *   9. mount on unformatted NVM returns CORRUPT
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "../include/nvlog.h"
#include "../backends/nvlog_posix.h"

/* ─── minimal test harness ────────────────────────────────────── */

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

#define TEST(name) fprintf(stdout, "\n[TEST] %s\n", name)

/* ─── helpers ─────────────────────────────────────────────────── */

#define NVM_SIZE  1024u
#define RING_SIZE ((uint32_t)(NVLOG_REGION_HEADER_SIZE + 2u * (NVLOG_RECORD_OVERHEAD + NVLOG_MAX_PAYLOAD) + 4096u))

static void make_ctx(nvlog_ctx_t *ctx, nvlog_posix_ctx_t *pctx, nvlog_hal_t *hal)
{
    nvlog_posix_open_ram(pctx, hal, NVM_SIZE);
    memset(ctx, 0, sizeof(*ctx));
}

/* ─── test 1: format + append + iter ─────────────────────────── */

static void test_basic(void)
{
    TEST("basic append + iter");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(ctx.media_class == NVLOG_MEDIA_CLASS_BYTE_WRITABLE);
    CHECK(ctx.program_unit == 1u);
    CHECK(ctx.erased_value == 0xFFu);
    CHECK(ctx.geometry_key == 0u);

    const char *msg = "hello nvlog";
    CHECK(nvlog_append(&ctx, msg, strlen(msg)) == NVLOG_OK);

    nvlog_iter_t   it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    CHECK(rec.seq == 0);
    CHECK((size_t)rec.len == strlen(msg));

    char buf[64] = {0};
    CHECK(nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) == NVLOG_OK);
    CHECK(memcmp(buf, msg, strlen(msg)) == 0);

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_NO_DATA);

    nvlog_posix_close(&pctx);
}

/* ─── test 2: multiple records, SEQ order ────────────────────── */

static void test_multiple(void)
{
    TEST("multiple records + SEQ order");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    nvlog_format(&ctx, &hal, NVM_SIZE);

    for (int i = 0; i < 5; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "rec%d", i);
        CHECK(nvlog_append(&ctx, buf, strlen(buf)) == NVLOG_OK);
    }

    nvlog_iter_t   it;
    nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx);

    for (uint32_t i = 0; i < 5; i++) {
        CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
        CHECK(rec.seq == i);
        char buf[16] = {0};
        nvlog_read_payload(&ctx, &rec, buf, sizeof(buf));
        char expected[8];
        snprintf(expected, sizeof(expected), "rec%u", i);
        CHECK(memcmp(buf, expected, strlen(expected)) == 0);
    }

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_NO_DATA);

    nvlog_posix_close(&pctx);
}

/* ─── test 3: corrupt CRC — record skipped by iter ───────────── */

static void test_corrupt_crc(void)
{
    TEST("corrupt CRC skipped by iter");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    nvlog_format(&ctx, &hal, NVM_SIZE);

    nvlog_append(&ctx, "good", 4);
    nvlog_append(&ctx, "bad",  3);
    nvlog_append(&ctx, "also-good", 9);

    /* corrupt the CRC of record 1 (seq=1) */
    /* record 0 offset: REGION_HEADER_SIZE
       record 1 offset: REGION_HEADER_SIZE + OVERHEAD + 4 */
    uint32_t r1_offset = (uint32_t)NVLOG_REGION_HEADER_SIZE
                       + NVLOG_RECORD_OVERHEAD + 4;
    uint32_t crc_offset = r1_offset + NVLOG_HEADER_SIZE + 3; /* after payload */
    uint8_t garbage = 0xAB;
    pctx.ram[crc_offset] = garbage;

    /* iter should yield seq=0, skip seq=1, yield seq=2 */
    nvlog_iter_t   it;
    nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx);

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    CHECK(rec.seq == 0);

    /* seq=1 is corrupt — iter skips it and continues */
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    CHECK(rec.seq == 2);

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_NO_DATA);

    nvlog_posix_close(&pctx);
}

/* ─── test 4: mount recovery ─────────────────────────────────── */

static void test_mount_recovery(void)
{
    TEST("mount recovery — reopen existing log");

    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    nvlog_posix_open_ram(&pctx, &hal, NVM_SIZE);

    /* first session: format + 3 appends */
    nvlog_ctx_t ctx1;
    nvlog_format(&ctx1, &hal, NVM_SIZE);
    nvlog_append(&ctx1, "A", 1);
    nvlog_append(&ctx1, "BB", 2);
    nvlog_append(&ctx1, "CCC", 3);

    /* second session: mount */
    nvlog_ctx_t ctx2;
    nvlog_ctx_init(&ctx2);
    CHECK(nvlog_mount(&ctx2, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(ctx2.next_seq == 3);

    /* append one more */
    CHECK(nvlog_append(&ctx2, "DDDD", 4) == NVLOG_OK);

    /* iterate all 4 */
    nvlog_iter_t   it;
    nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx2);

    uint32_t count = 0;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) count++;
    CHECK(count == 4);
    CHECK(rec.seq == 3);

    nvlog_posix_close(&pctx);
}

static void test_linear_remount_invalidates_iterator(void)
{
    TEST("linear remount invalidates iterator");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "A", 1) == NVLOG_OK);

    nvlog_iter_t it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);

    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_STALE);

    nvlog_posix_close(&pctx);
}

static void test_linear_mount_is_read_only(void)
{
    TEST("linear mount is read-only");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "A", 1) == NVLOG_OK);

    nvlog_posix_inject_fail_after(&pctx, 0);
    nvlog_ctx_t mounted;
    nvlog_ctx_init(&mounted);
    CHECK(nvlog_mount(&mounted, &hal, NVM_SIZE) == NVLOG_OK);
    nvlog_posix_inject_fail_after(&pctx, -1);

    nvlog_posix_close(&pctx);
}

/* ─── test 5: power-loss during append ───────────────────────── */

static void test_power_loss(void)
{
    TEST("power-loss during append — partial record invisible");

    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    nvlog_posix_open_ram(&pctx, &hal, NVM_SIZE);

    nvlog_ctx_t ctx;
    nvlog_format(&ctx, &hal, NVM_SIZE);
    nvlog_append(&ctx, "before", 6);

    /* inject power-loss: fail after 1 write (header written, payload not) */
    nvlog_posix_inject_fail_after(&pctx, 1);
    nvlog_status_t st = nvlog_append(&ctx, "interrupted", 11);
    CHECK(st == NVLOG_ERR_IO);

    /* disable injection, mount and recover */
    nvlog_posix_inject_fail_after(&pctx, -1);

    nvlog_ctx_t ctx2;
    nvlog_ctx_init(&ctx2);
    CHECK(nvlog_mount(&ctx2, &hal, NVM_SIZE) == NVLOG_OK);

    /* only "before" should be visible */
    nvlog_iter_t   it;
    nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx2);

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    CHECK(rec.seq == 0);

    char buf[16] = {0};
    nvlog_read_payload(&ctx2, &rec, buf, sizeof(buf));
    CHECK(memcmp(buf, "before", 6) == 0);

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_NO_DATA);
    CHECK(ctx2.next_seq == 1);

    nvlog_posix_close(&pctx);
}

/* ─── test 6: buffer full ─────────────────────────────────────── */

static void test_full(void)
{
    TEST("buffer full returns NVLOG_ERR_FULL");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    /* very small region: header + space for exactly one record */
    uint32_t small = (uint32_t)NVLOG_REGION_HEADER_SIZE + NVLOG_RECORD_OVERHEAD + 4;
    nvlog_posix_close(&pctx);
    nvlog_posix_open_ram(&pctx, &hal, small);

    nvlog_format(&ctx, &hal, small);
    CHECK(nvlog_append(&ctx, "ABCD", 4) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "X",    1) == NVLOG_ERR_FULL);

    nvlog_posix_close(&pctx);
}

/* ─── test 7: stats ───────────────────────────────────────────── */

static void test_stats(void)
{
    TEST("stats");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    nvlog_format(&ctx, &hal, NVM_SIZE);
    nvlog_append(&ctx, "hello", 5);
    nvlog_append(&ctx, "world", 5);

    nvlog_stats_t s;
    CHECK(nvlog_stats(&ctx, &s) == NVLOG_OK);
    CHECK(s.record_count == 2);
    CHECK(s.next_seq     == 2);
    CHECK(s.used_bytes   == 2 * (NVLOG_RECORD_OVERHEAD + 5));
    CHECK(s.free_bytes   == NVM_SIZE - NVLOG_REGION_HEADER_SIZE - s.used_bytes);

    nvlog_posix_close(&pctx);
}

/* ─── test 8: zero-length payload ────────────────────────────── */

static void test_zero_payload(void)
{
    TEST("zero-length payload");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    nvlog_format(&ctx, &hal, NVM_SIZE);
    CHECK(nvlog_append(&ctx, NULL, 0) == NVLOG_OK);

    nvlog_iter_t   it;
    nvlog_record_t rec;
    nvlog_iter_init(&it, &ctx);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    CHECK(rec.seq == 0);
    CHECK(rec.len == 0);

    nvlog_posix_close(&pctx);
}

/* ─── test 9: mount on unformatted NVM ───────────────────────── */

static void test_mount_unformatted(void)
{
    TEST("mount on unformatted NVM returns CORRUPT");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);
    /* pctx.ram is 0xFF — no region header written */

    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_CORRUPT);

    nvlog_posix_close(&pctx);
}

/* â”€â”€â”€ test 10: mode/version validation on superblocks â”€â”€â”€â”€â”€â”€â”€â”€ */

static void test_superblock_validation(void)
{
    TEST("superblock mode + version validation");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);

    /* Ring mount against linear media must reject the mode mismatch. */
    CHECK(nvlog_ring_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_MODE_MISMATCH);

    /* Reformat for ring and ensure linear mount rejects it. */
    CHECK(nvlog_ring_format(&ctx, &hal, RING_SIZE) == NVLOG_OK);
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_MODE_MISMATCH);

    /* Explicit v0.4 rejection: corrupt both superblocks to version 0x04. */
    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    pctx.ram[2] = 0x04u;
    pctx.ram[NVLOG_SUPERBLOCK_SIZE + 2] = 0x04u;
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_CORRUPT);

    nvlog_posix_close(&pctx);
}

static void test_read_failures_and_large_buffers(void)
{
    TEST("read failures and large payload buffers");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "payload", 7) == NVLOG_OK);

    nvlog_posix_inject_read_fail_after(&pctx, 0);
    nvlog_ctx_t mount_ctx;
    nvlog_ctx_init(&mount_ctx);
    CHECK(nvlog_mount(&mount_ctx, &hal, NVM_SIZE) == NVLOG_ERR_IO);
    nvlog_posix_inject_read_fail_after(&pctx, -1);

    CHECK(nvlog_mount(&mount_ctx, &hal, NVM_SIZE) == NVLOG_OK);

    nvlog_iter_t it;
    nvlog_record_t rec;
    CHECK(nvlog_iter_init(&it, &mount_ctx) == NVLOG_OK);

    nvlog_posix_inject_read_fail_after(&pctx, 0);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_IO);
    nvlog_posix_inject_read_fail_after(&pctx, -1);

    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    char *buf = (char *)malloc((size_t)UINT16_MAX + 32u);
    CHECK(buf != NULL);
    if (buf) {
        CHECK(nvlog_read_payload(&mount_ctx, &rec, buf, (size_t)UINT16_MAX + 1u) == NVLOG_OK);
        CHECK(memcmp(buf, "payload", 7) == 0);
        free(buf);
    }

    nvlog_posix_inject_read_fail_after(&pctx, 0);
    CHECK(nvlog_read_payload(&mount_ctx, &rec, (char[8]){0}, sizeof((char[8]){0})) == NVLOG_ERR_IO);
    nvlog_posix_inject_read_fail_after(&pctx, -1);

    nvlog_posix_close(&pctx);
}

static void test_record_statuses(void)
{
    TEST("record and mount verification statuses");

    nvlog_ctx_t       ctx;
    nvlog_posix_ctx_t pctx;
    nvlog_hal_t       hal;
    make_ctx(&ctx, &pctx, &hal);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);

    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE - 1u) == NVLOG_ERR_SIZE_MISMATCH);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + 1u] = 0x7Fu;
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_TYPE);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + 3u] = 0x7Fu;
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_FLAGS);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + 5u] = 0x01u;
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_RESERVED);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + 8u] ^= 0x01u;
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_GENERATION_MISMATCH);

    CHECK(nvlog_format(&ctx, &hal, NVM_SIZE) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "one", 3) == NVLOG_OK);
    pctx.ram[NVLOG_REGION_HEADER_SIZE + NVLOG_HEADER_SIZE + 1u] ^= 0x01u;
    CHECK(nvlog_mount(&ctx, &hal, NVM_SIZE) == NVLOG_ERR_CORRUPT);

    nvlog_posix_close(&pctx);
}

/* ─── main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("nvlog test suite\n");
    printf("=====================\n");

    test_basic();
    test_multiple();
    test_corrupt_crc();
    test_mount_recovery();
    test_linear_remount_invalidates_iterator();
    test_linear_mount_is_read_only();
    test_power_loss();
    test_full();
    test_stats();
    test_zero_payload();
    test_mount_unformatted();
    test_superblock_validation();
    test_read_failures_and_large_buffers();
    test_record_statuses();

    printf("\n=====================\n");
    printf("PASSED: %d\n", g_pass);
    printf("FAILED: %d\n", g_fail);
    printf("=====================\n");

    return g_fail == 0 ? 0 : 1;
}
