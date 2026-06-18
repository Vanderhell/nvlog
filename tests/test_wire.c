#include <stdio.h>
#include <string.h>

#include "../include/nvlog.h"
#include "../include/nvlog_wire.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

static void test_le_encoding(void)
{
    uint8_t buf[8];

    nvlog_store_u16le(buf, 0x1234u);
    nvlog_store_u32le(buf + 2, 0x89ABCDEFu);

    CHECK(buf[0] == 0x34u);
    CHECK(buf[1] == 0x12u);
    CHECK(buf[2] == 0xEFu);
    CHECK(buf[3] == 0xCDu);
    CHECK(buf[4] == 0xABu);
    CHECK(buf[5] == 0x89u);
    CHECK(nvlog_load_u16le(buf) == 0x1234u);
    CHECK(nvlog_load_u32le(buf + 2) == 0x89ABCDEFu);
}

static void test_checked_arithmetic(void)
{
    uint32_t out = 0;
    CHECK(nvlog_u32_add_checked(10u, 20u, &out) == 1);
    CHECK(out == 30u);
    CHECK(nvlog_u32_add_checked(UINT32_MAX, 1u, &out) == 0);
    CHECK(nvlog_u32_sub_checked(20u, 10u, &out) == 1);
    CHECK(out == 10u);
    CHECK(nvlog_u32_sub_checked(10u, 20u, &out) == 0);
}

static void test_api_misuse(void)
{
    nvlog_iter_t it;
    nvlog_record_t rec;
    nvlog_ctx_t ctx;
    uint8_t buf[16];
    nvlog_hal_t hal = {0};

    memset(&it, 0, sizeof(it));
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_ERR_NOT_MOUNTED);

    memset(&ctx, 0, sizeof(ctx));
    CHECK(nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) == NVLOG_ERR_NOT_MOUNTED);

    (void)hal;
}

int main(void)
{
    printf("nvlog wire helper test\n");
    test_le_encoding();
    test_checked_arithmetic();
    test_api_misuse();
    printf("PASSED: %d\n", g_pass);
    printf("FAILED: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
