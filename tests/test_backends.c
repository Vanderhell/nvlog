#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/nvlog.h"
#include "../include/nvlog_hal_flash.h"
#include "../backends/nvlog_posix.h"
#include "../backends/hal_examples/nvlog_hal_fram.h"
#include "../backends/hal_examples/nvlog_hal_eeprom.h"
#include "../backends/hal_examples/nvlog_hal_nor_spi.h"
#include "../backends/hal_examples/nvlog_espidf_partition.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

#define TEST(name) printf("\n[%s]\n", name)

static int g_pass = 0;
static int g_fail = 0;

/* ---------------- POSIX ---------------- */

static void test_posix_file_contract(void)
{
    TEST("posix file contract");

    nvlog_posix_ctx_t pctx;
    nvlog_hal_t hal;
    CHECK(nvlog_posix_open_ram(&pctx, &hal, 256u) == 0);

    nvlog_ctx_t ctx;
    CHECK(nvlog_format(&ctx, &hal, 256u) == NVLOG_OK);
    CHECK(nvlog_append(&ctx, "abc", 3) == NVLOG_OK);

    nvlog_record_t rec;
    nvlog_iter_t it;
    CHECK(nvlog_iter_init(&it, &ctx) == NVLOG_OK);
    CHECK(nvlog_iter_next(&it, &rec) == NVLOG_OK);
    char buf[8] = {0};
    CHECK(nvlog_read_payload(&ctx, &rec, buf, sizeof(buf)) == NVLOG_OK);
    CHECK(memcmp(buf, "abc", 3) == 0);

    CHECK(hal.read(255u, buf, 1u, hal.user) == 0);
    CHECK(hal.read(256u, buf, 1u, hal.user) == -1);
    nvlog_posix_inject_read_fail_after(&pctx, 0);
    CHECK(hal.read(0, buf, 1u, hal.user) == -1);
    nvlog_posix_close(&pctx);
}

/* ---------------- FRAM SPI / I2C ---------------- */

typedef struct {
    uint8_t mem[1024];
    uint8_t cs;
    uint8_t phase;
    uint8_t opcode;
    uint32_t addr;
    uint8_t addr_bytes;
    uint32_t tx_count;
    uint32_t wren_count;
    uint32_t write_count;
    uint32_t read_count;
    int fail_wren_once;
    int fail_next_transfer;
} fram_spi_mock_t;

static void fram_spi_cs(int assert, void *user)
{
    fram_spi_mock_t *m = (fram_spi_mock_t *)user;
    m->cs = (uint8_t)assert;
}

static int fram_spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len, void *user)
{
    fram_spi_mock_t *m = (fram_spi_mock_t *)user;
    m->tx_count++;
    if (m->fail_next_transfer) {
        m->fail_next_transfer = 0;
        return -1;
    }
    if (!tx && !rx) return -1;

    if (tx && len == 1u && tx[0] == NVLOG_FRAM_SPI_OP_WREN) {
        m->wren_count++;
        if (m->fail_wren_once) {
            m->fail_wren_once = 0;
            return -1;
        }
        m->phase = 1;
        return 0;
    }

    if (tx && m->phase == 3 && m->opcode == NVLOG_FRAM_SPI_OP_WRITE) {
        memcpy(m->mem + m->addr, tx, len);
        m->write_count++;
        m->phase = 0;
        return 0;
    }

    if (rx && m->phase == 3 && m->opcode == NVLOG_FRAM_SPI_OP_READ) {
        memcpy(rx, m->mem + m->addr, len);
        m->read_count++;
        m->phase = 0;
        return 0;
    }

    if (tx && len >= 2u &&
        (tx[0] == NVLOG_FRAM_SPI_OP_READ || tx[0] == NVLOG_FRAM_SPI_OP_WRITE)) {
        m->opcode = tx[0];
        m->phase = 3;
        m->addr = 0;
        m->addr_bytes = (uint8_t)(len - 1u);
        for (uint32_t i = 1; i < len; i++)
            m->addr = (m->addr << 8) | tx[i];
        return 0;
    }

    return 0;
}

typedef struct {
    uint8_t mem[1024];
    uint32_t last_dev_addr;
    uint32_t last_addr;
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t busy_polls;
    uint32_t busy_threshold;
    uint32_t write_data_calls;
} fram_i2c_mock_t;

static int fram_i2c_write(uint8_t dev_addr, const uint8_t *buf, uint32_t len, void *user)
{
    fram_i2c_mock_t *m = (fram_i2c_mock_t *)user;
    m->last_dev_addr = dev_addr;
    if (len == 0u) {
        if (m->busy_polls < m->busy_threshold) {
            m->busy_polls++;
            return -1;
        }
        return 0;
    }
    if (len >= 2u && len <= 3u) {
        m->last_addr = 0;
        for (uint32_t i = 0; i < len; i++)
            m->last_addr = (m->last_addr << 8) | buf[i];
        return 0;
    }
    if (len > 3u) {
        uint32_t addr = 0;
        addr = (uint32_t)buf[0] << 8 | (uint32_t)buf[1];
        memcpy(m->mem + addr, buf + 2, len - 2u);
        m->write_data_calls++;
        return 0;
    }
    return 0;
}

static int fram_i2c_read(uint8_t dev_addr, uint8_t *buf, uint32_t len, void *user)
{
    fram_i2c_mock_t *m = (fram_i2c_mock_t *)user;
    m->last_dev_addr = dev_addr;
    memcpy(buf, m->mem + m->last_addr, len);
    m->read_calls++;
    return 0;
}

static void test_fram_spi_protocol(void)
{
    TEST("fram spi protocol");

    fram_spi_mock_t mock;
    memset(&mock, 0, sizeof(mock));

    nvlog_fram_ctx_t ctx;
    nvlog_hal_t hal;
    nvlog_fram_spi_glue_t glue = { fram_spi_cs, fram_spi_transfer, &mock, 2u };
    CHECK(nvlog_fram_init_spi(&ctx, &hal, &glue) == NVLOG_OK);

    uint8_t payload[] = { 1, 2, 3, 4 };
    CHECK(hal.write(0x123u, payload, sizeof(payload), hal.user) == 0);
    CHECK(mock.wren_count == 1u);
    CHECK(mock.write_count == 1u);
    CHECK(mock.mem[0x123u] == 1u);

    mock.fail_wren_once = 1;
    CHECK(hal.write(0x10u, payload, sizeof(payload), hal.user) == -1);
    CHECK(mock.write_count == 1u);

    uint8_t readbuf[4] = {0};
    CHECK(hal.read(0x123u, readbuf, sizeof(readbuf), hal.user) == 0);
    CHECK(memcmp(readbuf, payload, sizeof(payload)) == 0);
    CHECK(mock.read_count == 1u);
}

static void test_fram_i2c_protocol(void)
{
    TEST("fram i2c protocol");

    fram_i2c_mock_t mock;
    memset(&mock, 0, sizeof(mock));

    nvlog_fram_ctx_t ctx;
    nvlog_hal_t hal;
    nvlog_fram_i2c_glue_t glue = { fram_i2c_write, fram_i2c_read, &mock, 0x50u, 2u };
    CHECK(nvlog_fram_init_i2c(&ctx, &hal, &glue) == NVLOG_OK);

    uint8_t payload[] = { 9, 8, 7 };
    CHECK(hal.write(0x20u, payload, sizeof(payload), hal.user) == 0);
    CHECK(mock.write_data_calls == 1u);
    CHECK(hal.read(0x20u, payload, sizeof(payload), hal.user) == 0);
    CHECK(mock.read_calls == 1u);
    CHECK(mock.last_dev_addr == 0x50u);
}

/* ---------------- EEPROM ---------------- */

typedef struct {
    uint8_t mem[256];
    uint32_t busy_threshold;
    uint32_t busy_polls;
    uint32_t write_calls;
    uint32_t ack_polls;
    uint32_t call_count;
    uint32_t last_addr;
    uint32_t page_writes[8];
    uint32_t page_write_count;
} eeprom_mock_t;

static int eeprom_i2c_write(uint8_t dev_addr, const uint8_t *buf, uint32_t len, void *user)
{
    eeprom_mock_t *m = (eeprom_mock_t *)user;
    (void)dev_addr;
    m->call_count++;
    if (len == 0u) {
        m->ack_polls++;
        if (m->busy_polls < m->busy_threshold) {
            m->busy_polls++;
            return -1;
        }
        return 0;
    }
    m->last_addr = ((uint32_t)buf[0] << 8) | (uint32_t)buf[1];
    if (len > 2u && m->page_write_count < 8u)
        m->page_writes[m->page_write_count++] = len;
    if (len > 2u)
        memcpy(m->mem + m->last_addr, buf + 2u, len - 2u);
    m->write_calls++;
    return 0;
}

static int eeprom_i2c_read(uint8_t dev_addr, const uint8_t *addr_buf,
                           uint8_t addr_len, uint8_t *buf, uint32_t len, void *user)
{
    eeprom_mock_t *m = (eeprom_mock_t *)user;
    (void)dev_addr;
    m->last_addr = 0;
    for (uint32_t i = 0; i < addr_len; i++)
        m->last_addr = (m->last_addr << 8) | addr_buf[i];
    memcpy(buf, m->mem + m->last_addr, len);
    return 0;
}

static void eeprom_delay(uint32_t ms, void *user)
{
    (void)ms;
    (void)user;
}

static void test_eeprom_protocol(void)
{
    TEST("eeprom protocol");

    eeprom_mock_t mock;
    memset(&mock, 0, sizeof(mock));

    nvlog_eeprom_ctx_t ctx;
    nvlog_hal_t hal;
    nvlog_eeprom_glue_t glue = { eeprom_i2c_write, eeprom_i2c_read, eeprom_delay, &mock };
    nvlog_eeprom_cfg_t cfg = { .dev_addr = 0x50u, .addr_bytes = 2u, .page_size = 8u, .capacity = 64u, .wr_timeout_ms = 3u };
    CHECK(nvlog_eeprom_init(&ctx, &hal, &glue, &cfg) == NVLOG_OK);

    uint8_t payload[10];
    for (uint32_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(0xA0u + i);
    CHECK(hal.write(6u, payload, sizeof(payload), hal.user) == 0);
    CHECK(mock.write_calls == 2u);
    CHECK(mock.page_writes[0] >= 4u);
    CHECK(mock.page_writes[1] >= 4u);
    CHECK(mock.ack_polls >= 2u);
    CHECK(memcmp(mock.mem + 6u, payload, sizeof(payload)) == 0);

    mock.busy_threshold = 100u;
    CHECK(hal.write(0u, payload, 2u, hal.user) == -1);
}

/* ---------------- SPI NOR ---------------- */

typedef struct {
    uint8_t mem[1024];
    uint8_t cs;
    uint8_t pending_cmd;
    uint32_t pending_addr;
    uint32_t pending_len;
    uint32_t wren_count;
    uint32_t pp_count;
    uint32_t rdsr_count;
    uint32_t read_count;
    uint32_t write_count;
    uint8_t busy_reads;
} nor_mock_t;

static void nor_cs(int assert, void *user)
{
    nor_mock_t *m = (nor_mock_t *)user;
    m->cs = (uint8_t)assert;
}

static int nor_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len, void *user)
{
    nor_mock_t *m = (nor_mock_t *)user;
    if (tx && len == 1u) {
        m->pending_cmd = tx[0];
        if (tx[0] == NOR_OP_WREN) {
            m->wren_count++;
            m->pending_len = 0u;
            m->pending_addr = 0u;
        }
        if (tx[0] == NOR_OP_RDSR) m->rdsr_count++;
        if (tx[0] == NOR_OP_RDID) m->pending_addr = 0u;
        return 0;
    }
    if (tx && len >= 4u && m->pending_cmd == NOR_OP_PP && m->pending_len == 0u) {
        m->pending_addr = ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | (uint32_t)tx[3];
        m->pending_len = 1u;
        return 0;
    }
    if (tx && m->pending_cmd == NOR_OP_PP && len > 0u && m->pending_len > 0u) {
        memcpy(m->mem + m->pending_addr, tx, len);
        m->pending_addr += len;
        m->pending_len += len;
        m->write_count++;
        m->pending_cmd = 0u;
        m->pending_len = 0u;
        return 0;
    }
    if (tx && len >= 4u && m->pending_cmd == NOR_OP_READ) {
        m->pending_addr = ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | (uint32_t)tx[3];
        return 0;
    }
    if (rx && len == 1u && m->pending_cmd == NOR_OP_RDSR) {
        rx[0] = m->busy_reads ? NOR_SR_WIP : 0u;
        if (m->busy_reads) m->busy_reads--;
        return 0;
    }
    if (rx && m->pending_cmd == NOR_OP_READ && len > 0u) {
        memcpy(rx, m->mem + m->pending_addr, len);
        m->read_count++;
        return 0;
    }
    if (rx && m->pending_cmd == NOR_OP_RDID && len == 3u) {
        rx[0] = 0xEFu;
        rx[1] = 0x40u;
        rx[2] = 0x17u;
        return 0;
    }
    return 0;
}

static void nor_delay(uint32_t ms, void *user)
{
    (void)ms;
    (void)user;
}

static void test_nor_protocol(void)
{
    TEST("spi nor protocol");

    nor_mock_t mock;
    memset(&mock, 0, sizeof(mock));

    nvlog_nor_spi_ctx_t ctx;
    nvlog_hal_flash_t flash;
    nvlog_nor_spi_glue_t glue = { nor_cs, nor_transfer, nor_delay, &mock, 3u, 4u };
    CHECK(nvlog_nor_spi_init(&ctx, &flash, &glue) == NVLOG_OK);

    uint8_t payload[300];
    for (uint32_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    mock.busy_reads = 2u;
    CHECK(flash.base.write(250u, payload, sizeof(payload), flash.base.user) == 0);
    CHECK(mock.wren_count >= 2u);
    CHECK(mock.rdsr_count >= 1u);

    uint8_t id[3] = {0xEFu, 0x40u, 0x17u};
    CHECK(nvlog_nor_spi_read_jedec_id(&ctx, id) == NVLOG_OK);
    CHECK(id[0] == 0xEFu && id[1] == 0x40u && id[2] == 0x17u);
}

/* ---------------- ESP-IDF partition ---------------- */

typedef struct {
    uint8_t *mem;
    uint32_t size;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t erase_calls;
} esp_mock_t;

typedef int esp_err_t;
#define ESP_OK 0

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *dst, size_t size)
{
    esp_mock_t *m = (esp_mock_t *)partition;
    m->read_calls++;
    if (offset + size > m->size) return -1;
    memcpy(dst, m->mem + offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *src, size_t size)
{
    esp_mock_t *m = (esp_mock_t *)partition;
    m->write_calls++;
    if (offset + size > m->size) return -1;
    memcpy(m->mem + offset, src, size);
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size)
{
    esp_mock_t *m = (esp_mock_t *)partition;
    m->erase_calls++;
    if (offset + size > m->size) return -1;
    memset(m->mem + offset, 0xFF, size);
    return ESP_OK;
}

static void test_espidf_partition_protocol(void)
{
    TEST("espidf partition protocol");

    esp_mock_t mock;
    memset(&mock, 0, sizeof(mock));
    uint8_t storage[64];
    memset(storage, 0xFF, sizeof(storage));
    mock.mem = storage;
    mock.size = sizeof(storage);

    nvlog_espidf_partition_ctx_t ctx;
    nvlog_hal_flash_t flash;
    CHECK(nvlog_espidf_partition_init(&ctx, &flash, (const esp_partition_t *)&mock, sizeof(storage), 16u, 4u) == NVLOG_OK);

    uint8_t payload[8] = {1,2,3,4,5,6,7,8};
    CHECK(flash.base.write(4u, payload, sizeof(payload), flash.base.user) == 0);
    CHECK(flash.base.read(4u, payload, sizeof(payload), flash.base.user) == 0);
    CHECK(flash.erase(16u, 16u, flash.user) == 0);
    CHECK(mock.write_calls == 1u);
    CHECK(mock.read_calls == 1u);
    CHECK(mock.erase_calls == 1u);
    CHECK(flash.base.write(65u, payload, 1u, flash.base.user) == -1);
}

int main(void)
{
    printf("nvlog backend protocol tests\n");
    test_posix_file_contract();
    test_fram_spi_protocol();
    test_fram_i2c_protocol();
    test_eeprom_protocol();
    test_nor_protocol();
    test_espidf_partition_protocol();
    printf("PASSED: %d\nFAILED: %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
