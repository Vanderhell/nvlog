/**
 * tests/test_flash.c — NOR flash backend tests
 *
 * Tests:
 *   FL-01  format erases region (all 0xFF after format, then header written)
 *   FL-02  verify_erased detects non-0xFF bytes
 *   FL-03  basic append + mount on simulated NOR flash
 *   FL-04  NOR physics: write 0→1 without erase triggers violation
 *   FL-05  format after use: re-erase clears records, fresh log works
 *   FL-06  power-loss during NOR write: record invisible after mount
 *   FL-07  power-loss during erase (format): mount returns CORRUPT
 *   FL-08  erase counter: each sector erased exactly once per format
 *   FL-09  region_size not multiple of erase_size → NVLOG_ERR_PARAM
 *   FL-10  many appends filling region → correct count after mount
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/nvlog.h"
#include "../include/nvlog_hal_flash.h"
#include "../include/nvlog_wire.h"
#include "../backends/nvlog_flash_sim.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

#define TEST(name) fprintf(stdout, "\n[FL] %s\n", name)

#define REGION_SIZE   (4 * 4096u)   /* 4 sectors of 4KB */
#define SECTOR_SIZE   4096u

static uint32_t flash_geometry_key(uint32_t erase_size, uint32_t prog_size)
{
    return ((erase_size / 1024u) << 8) | (prog_size & 0xFFu);
}

/* ─── helpers ─────────────────────────────────────────────────── */

static void fresh_sim(nvlog_flash_sim_ctx_t *sim, nvlog_hal_flash_t *flash)
{
    nvlog_flash_sim_open(sim, flash, REGION_SIZE, SECTOR_SIZE);
}

static uint32_t count_records_flash(nvlog_ctx_t *ctx)
{
    nvlog_iter_t it; nvlog_record_t rec;
    nvlog_iter_init(&it, ctx);
    uint32_t n = 0;
    while (nvlog_iter_next(&it, &rec) == NVLOG_OK) n++;
    return n;
}

/* ─── FL-01: format leaves valid region header, rest 0xFF ─────── */

static void test_fl01(void)
{
    TEST("FL-01: format erases region and writes header");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    CHECK(nvlog_flash_format(&ctx, &flash, REGION_SIZE) == NVLOG_OK);
    CHECK(ctx.media_class == NVLOG_MEDIA_CLASS_ERASE_BEFORE_WRITE);
    CHECK(ctx.program_unit == 1u);
    CHECK(ctx.erased_value == 0xFFu);
    CHECK(ctx.geometry_key == flash_geometry_key(SECTOR_SIZE, 1u));

    /* first 64 bytes = redundant A/B superblock pair */
    uint8_t sb0[NVLOG_SUPERBLOCK_SIZE];
    uint8_t sb1[NVLOG_SUPERBLOCK_SIZE];
    flash.base.read(0, sb0, sizeof(sb0), flash.base.user);
    flash.base.read(NVLOG_SUPERBLOCK_SIZE, sb1, sizeof(sb1), flash.base.user);

    CHECK(nvlog_load_u32le(sb0 + 0) == NVLOG_MEDIA_MAGIC);
    CHECK(nvlog_load_u16le(sb0 + 4) == NVLOG_MEDIA_VERSION);
    CHECK(sb0[6] == NVLOG_MODE_LINEAR);
    CHECK(nvlog_load_u32le(sb0 + 8) == REGION_SIZE);
    CHECK(nvlog_load_u32le(sb1 + 0) == NVLOG_MEDIA_MAGIC);
    CHECK(nvlog_load_u16le(sb1 + 4) == NVLOG_MEDIA_VERSION);
    CHECK(sb1[6] == NVLOG_MODE_LINEAR);
    CHECK(nvlog_load_u32le(sb1 + 8) == REGION_SIZE);

    /* record area after the metadata header remains erased */
    uint8_t byte;
    flash.base.read(NVLOG_REGION_HEADER_SIZE, &byte, 1, flash.base.user);
    CHECK(byte == 0xFF);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-02: verify_erased detects dirty bytes ────────────────── */

static void test_fl02(void)
{
    TEST("FL-02: verify_erased detects non-0xFF");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    /* fresh sim = all 0xFF */
    CHECK(nvlog_flash_verify_erased(&flash, REGION_SIZE) == NVLOG_OK);

    /* dirty one byte by simulating a write (bypass physics via direct mem set) */
    sim.mem[100] = 0xAB;
    CHECK(nvlog_flash_verify_erased(&flash, REGION_SIZE) == NVLOG_ERR_CORRUPT);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-03: basic append + mount on NOR sim ──────────────────── */

static void test_fl03(void)
{
    TEST("FL-03: basic append + mount on NOR flash sim");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    nvlog_flash_format(&ctx, &flash, REGION_SIZE);

    CHECK(nvlog_append(&ctx, "hello", 5) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "world", 5) == NVLOG_OK);

    /* mount in new ctx */
    nvlog_ctx_t ctx2;
    CHECK(nvlog_mount(&ctx2, &flash.base, REGION_SIZE) == NVLOG_OK);
    CHECK(count_records_flash(&ctx2) == 2);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-04: NOR physics — write 0→1 detected ────────────────── */

static void test_fl04(void)
{
    TEST("FL-04: write 0→1 without erase is a violation");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    /* write 0x00 (all bits 0) to a byte — valid 1→0 */
    uint8_t zero = 0x00;
    flash.base.write(0, &zero, 1, flash.base.user);
    CHECK(sim.mem[0] == 0x00);
    CHECK(sim.bit_flip_violations == 0);

    /* now try to write 0xFF back — illegal 0→1 */
    uint8_t ff = 0xFF;
    int rc = flash.base.write(0, &ff, 1, flash.base.user);
    CHECK(rc != 0);  /* must fail */
    CHECK(sim.bit_flip_violations == 1);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-05: re-format after use clears records ───────────────── */

static void test_fl05(void)
{
    TEST("FL-05: format after use erases + fresh log works");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    nvlog_flash_format(&ctx, &flash, REGION_SIZE);
    nvlog_append(&ctx, "old", 3);
    nvlog_append(&ctx, "data", 4);

    /* re-format */
    nvlog_ctx_t ctx2;
    CHECK(nvlog_flash_format(&ctx2, &flash, REGION_SIZE) == NVLOG_OK);

    /* verify no records */
    CHECK(count_records_flash(&ctx2) == 0);

    /* can append fresh records */
    CHECK(nvlog_append(&ctx2, "new", 3) == NVLOG_OK);
    CHECK(count_records_flash(&ctx2) == 1);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-06: power-loss during NOR write ─────────────────────── */

static void test_fl06(void)
{
    TEST("FL-06: power-loss during NOR write → record invisible");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    nvlog_flash_format(&ctx, &flash, REGION_SIZE);
    nvlog_append(&ctx, "safe", 4);

    /* fail after header write, before CRC commit */
    nvlog_flash_sim_inject_write_fail(&sim, 0);
    nvlog_append(&ctx, "lost", 4);
    nvlog_flash_sim_inject_write_fail(&sim, -1);

    nvlog_ctx_t ctx2;
    CHECK(nvlog_mount(&ctx2, &flash.base, REGION_SIZE) == NVLOG_OK);
    CHECK(count_records_flash(&ctx2) == 1);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-07: power-loss during erase (format) ────────────────── */

static void test_fl07(void)
{
    TEST("FL-07: power-loss during format erase → mount returns CORRUPT");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    /* fail on first erase → region header never written */
    nvlog_flash_sim_inject_erase_fail(&sim, 0);
    nvlog_ctx_t ctx;
    nvlog_status_t st = nvlog_flash_format(&ctx, &flash, REGION_SIZE);
    CHECK(st == NVLOG_ERR_IO);
    nvlog_flash_sim_inject_erase_fail(&sim, -1);

    /* mount should fail — no valid region header */
    nvlog_ctx_t ctx2;
    CHECK(nvlog_mount(&ctx2, &flash.base, REGION_SIZE) == NVLOG_ERR_CORRUPT);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-08: erase counter — one erase per sector per format ─── */

static void test_fl08(void)
{
    TEST("FL-08: each sector erased exactly once per format");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    nvlog_flash_format(&ctx, &flash, REGION_SIZE);

    uint32_t sectors = REGION_SIZE / SECTOR_SIZE;
    for (uint32_t i = 0; i < sectors; i++)
        CHECK(sim.erase_counts[i] == 1);

    /* second format: each sector erased once more */
    nvlog_flash_format(&ctx, &flash, REGION_SIZE);
    for (uint32_t i = 0; i < sectors; i++)
        CHECK(sim.erase_counts[i] == 2);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-09: bad region_size param ───────────────────────────── */

static void test_fl09(void)
{
    TEST("FL-09: region_size not sector-aligned → NVLOG_ERR_PARAM");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    /* 4097 is not a multiple of 4096 */
    CHECK(nvlog_flash_format(&ctx, &flash, 4097u) == NVLOG_ERR_PARAM);

    nvlog_flash_sim_close(&sim);
}

/* ─── FL-10: fill region with many records ────────────────────── */

static void test_fl10(void)
{
    TEST("FL-10: fill region, correct count + FULL on overflow");
    nvlog_flash_sim_ctx_t sim; nvlog_hal_flash_t flash;
    fresh_sim(&sim, &flash);

    nvlog_ctx_t ctx;
    nvlog_flash_format(&ctx, &flash, REGION_SIZE);

    /* record size: overhead (12B) + payload (4B) = 16B per record
       available: REGION_SIZE - region_header (8B) = 16376B
       16376 / 16 = 1023 records */
    uint32_t written = 0;
    nvlog_status_t st = NVLOG_OK;
    while (st == NVLOG_OK) {
        st = nvlog_append(&ctx, "ABCD", 4);
        if (st == NVLOG_OK) written++;
    }
    CHECK(st == NVLOG_ERR_FULL);
    CHECK(written > 0);

    /* mount and verify count matches */
    nvlog_ctx_t ctx2;
    nvlog_mount(&ctx2, &flash.base, REGION_SIZE);
    CHECK(count_records_flash(&ctx2) == written);

    nvlog_flash_sim_close(&sim);
}

/* â”€â”€â”€ FL-11: supported program-unit sizes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static void test_fl11(void)
{
    TEST("FL-11: supported program-unit sizes");

    const uint32_t units[] = {1u, 4u, 8u, 32u};
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        nvlog_flash_sim_ctx_t sim;
        nvlog_hal_flash_t flash;
        nvlog_flash_sim_cfg_t cfg = {0};
        cfg.capacity = REGION_SIZE;
        cfg.erased_value = 0xFFu;
        cfg.erase_unit = SECTOR_SIZE;
        cfg.program_unit = units[i];
        cfg.program_alignment = units[i];
        cfg.max_transfer = 0u;
        cfg.sector_size = SECTOR_SIZE;
        CHECK(nvlog_flash_sim_open_cfg(&sim, &flash, &cfg) == 0);

        nvlog_ctx_t ctx;
        CHECK(nvlog_flash_format(&ctx, &flash, REGION_SIZE) == NVLOG_OK);
        CHECK(ctx.media_class == NVLOG_MEDIA_CLASS_ERASE_BEFORE_WRITE);
        CHECK(ctx.program_unit == (uint8_t)units[i]);
        CHECK(ctx.erased_value == 0xFFu);
        CHECK(ctx.geometry_key == flash_geometry_key(SECTOR_SIZE, units[i]));
        CHECK(nvlog_flash_verify_erased(&flash, REGION_SIZE) == NVLOG_OK);

        nvlog_flash_sim_close(&sim);
    }

    nvlog_flash_sim_ctx_t sim;
    nvlog_hal_flash_t flash;
    nvlog_flash_sim_cfg_t cfg = {0};
    cfg.capacity = REGION_SIZE;
    cfg.erased_value = 0xFFu;
    cfg.erase_unit = SECTOR_SIZE;
    cfg.program_unit = 2u;
    cfg.program_alignment = 2u;
    cfg.max_transfer = 0u;
    cfg.sector_size = SECTOR_SIZE;
    CHECK(nvlog_flash_sim_open_cfg(&sim, &flash, &cfg) == 0);
    nvlog_ctx_t ctx;
    CHECK(nvlog_flash_format(&ctx, &flash, REGION_SIZE) == NVLOG_ERR_PARAM);
    nvlog_flash_sim_close(&sim);
}

static void test_fl12(void)
{
    TEST("FL-12: partial flash program returns old-or-new state");

    const uint32_t units[] = {1u, 4u, 8u, 32u};
    const uint32_t old_len = 5u;
    const uint32_t new_lens[] = {1u, 3u, 7u, 11u, 19u};
    for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
        for (size_t n = 0; n < sizeof(new_lens) / sizeof(new_lens[0]); n++) {
            nvlog_flash_sim_ctx_t sim;
            nvlog_hal_flash_t flash;
            nvlog_flash_sim_cfg_t cfg = {0};
            cfg.capacity = REGION_SIZE;
            cfg.erased_value = 0xFFu;
            cfg.erase_unit = SECTOR_SIZE;
            cfg.program_unit = units[u];
            cfg.program_alignment = units[u];
            cfg.max_transfer = 0u;
            cfg.sector_size = SECTOR_SIZE;
            CHECK(nvlog_flash_sim_open_cfg(&sim, &flash, &cfg) == 0);

            nvlog_ctx_t ctx;
            CHECK(nvlog_flash_format(&ctx, &flash, REGION_SIZE) == NVLOG_OK);

            uint8_t old_payload[64];
            uint8_t new_payload[64];
            uint32_t new_len = new_lens[n] + 1u;
            if (new_len > sizeof(new_payload))
                new_len = new_lens[n];
            for (uint32_t i = 0; i < old_len; i++) old_payload[i] = (uint8_t)(0x10u + i);
            for (uint32_t i = 0; i < new_len; i++) new_payload[i] = (uint8_t)(0x80u + i);

            CHECK(nvlog_append(&ctx, old_payload, old_len) == NVLOG_OK);

            uint32_t record_alloc = (uint32_t)((NVLOG_RECORD_OVERHEAD + new_len + (units[u] - 1u)) / units[u]) * units[u];
            const uint32_t cuts[] = {0u, 1u, units[u] - 1u, units[u], units[u] + 1u, record_alloc - 1u, record_alloc};
            for (size_t c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
                nvlog_flash_sim_reset(&sim);
                CHECK(nvlog_flash_format(&ctx, &flash, REGION_SIZE) == NVLOG_OK);
                CHECK(nvlog_append(&ctx, old_payload, old_len) == NVLOG_OK);
                nvlog_flash_sim_inject_write_partial(&sim, (int32_t)cuts[c]);
                nvlog_status_t st = nvlog_append(&ctx, new_payload, new_len);
                nvlog_flash_sim_inject_write_partial(&sim, -1);

                nvlog_ctx_t mounted;
                CHECK(nvlog_mount(&mounted, &flash.base, REGION_SIZE) == NVLOG_OK);

                uint32_t count = count_records_flash(&mounted);
                CHECK(count == 1u || count == 2u);
                if (count == 1u) {
                    CHECK(st == NVLOG_ERR_IO || st == NVLOG_OK);
                    nvlog_iter_t it;
                    nvlog_record_t rec;
                    CHECK(nvlog_iter_init(&it, &mounted) == NVLOG_OK);
                    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
                    uint8_t buf[64];
                    CHECK(nvlog_read_payload(&mounted, &rec, buf, sizeof(buf)) == NVLOG_OK);
                    CHECK(memcmp(buf, old_payload, old_len) == 0);
                } else {
                    nvlog_iter_t it;
                    nvlog_record_t rec;
                    CHECK(nvlog_iter_init(&it, &mounted) == NVLOG_OK);
                    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
                    uint8_t buf[64];
                    CHECK(nvlog_read_payload(&mounted, &rec, buf, sizeof(buf)) == NVLOG_OK);
                    CHECK(memcmp(buf, old_payload, old_len) == 0);
                    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
                    CHECK(nvlog_read_payload(&mounted, &rec, buf, sizeof(buf)) == NVLOG_OK);
                    CHECK(memcmp(buf, new_payload, new_len) == 0);
                    CHECK(st == NVLOG_OK || st == NVLOG_ERR_IO);
                }

                CHECK(nvlog_append(&mounted, new_payload, new_len) == NVLOG_OK);
                nvlog_ctx_t remounted;
                memset(&remounted, 0, sizeof(remounted));
                remounted.session_id = 0xF12F1200u | (uint32_t)u;
                CHECK(nvlog_mount(&remounted, &flash.base, REGION_SIZE) == NVLOG_OK);
                nvlog_iter_t it;
                nvlog_record_t last = {0};
                nvlog_record_t rec;
                uint32_t seen = 0;
                CHECK(nvlog_iter_init(&it, &remounted) == NVLOG_OK);
                while (nvlog_iter_next(&it, &rec) == NVLOG_OK) {
                    last = rec;
                    seen++;
                }
                CHECK(seen >= 1u);
                uint8_t buf[64];
                CHECK(nvlog_read_payload(&remounted, &last, buf, sizeof(buf)) == NVLOG_OK);
                CHECK(memcmp(buf, new_payload, new_len) == 0);
            }
            nvlog_flash_sim_close(&sim);
        }
    }
}

/* ─── main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("nvlog flash backend test suite\n");
    printf("=======================================\n");

    test_fl01();
    test_fl02();
    test_fl03();
    test_fl04();
    test_fl05();
    test_fl06();
    test_fl07();
    test_fl08();
    test_fl09();
    test_fl10();
    test_fl11();
    test_fl12();

    printf("\n=======================================\n");
    printf("PASSED: %d\n", g_pass);
    printf("FAILED: %d\n", g_fail);
    printf("=======================================\n");

    return g_fail == 0 ? 0 : 1;
}
