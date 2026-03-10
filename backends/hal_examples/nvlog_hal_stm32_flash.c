/**
 * nvlog_hal_stm32_flash.c — STM32 internal flash HAL
 *
 * Direct register access — no HAL library dependency.
 * CMSIS device headers required (for register definitions).
 *
 * Supported families:
 *   NVLOG_STM32_FLASH_F4  — STM32F4xx
 *   NVLOG_STM32_FLASH_L4  — STM32L4xx
 *   NVLOG_STM32_FLASH_H7  — STM32H7xx
 *
 * ⚠️  This file uses direct memory-mapped register writes.
 *     Interrupts that touch flash should be disabled during
 *     erase/program operations (or run from RAM).
 *     See your device reference manual section "Flash interface".
 */

#include "nvlog_hal_stm32_flash.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════
 * REGISTER DEFINITIONS
 * (subset needed for nvlog — avoids full CMSIS dependency for
 *  reference/example purposes; replace with your device header)
 * ════════════════════════════════════════════════════════════════ */

#if defined(NVLOG_STM32_FLASH_F4)

#define FLASH_BASE_ADDR     0x40023C00UL
#define FLASH_ACR           (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x00))
#define FLASH_KEYR          (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x04))
#define FLASH_SR            (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x0C))
#define FLASH_CR            (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x10))

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL
#define FLASH_SR_BSY        (1u << 16)
#define FLASH_SR_PGSERR     (1u << 7)
#define FLASH_SR_PGPERR     (1u << 6)
#define FLASH_SR_PGAERR     (1u << 5)
#define FLASH_SR_WRPERR     (1u << 4)
#define FLASH_SR_OPERR      (1u << 1)
#define FLASH_SR_EOP        (1u << 0)
#define FLASH_SR_ERRMASK    (FLASH_SR_PGSERR | FLASH_SR_PGPERR | \
                             FLASH_SR_PGAERR | FLASH_SR_WRPERR | FLASH_SR_OPERR)
#define FLASH_CR_PG         (1u << 0)
#define FLASH_CR_SER        (1u << 1)
#define FLASH_CR_MER        (1u << 2)
#define FLASH_CR_SNB_SHIFT  3
#define FLASH_CR_PSIZE_X32  (2u << 8)
#define FLASH_CR_STRT       (1u << 16)
#define FLASH_CR_LOCK       (1u << 31)

#elif defined(NVLOG_STM32_FLASH_L4)

#define FLASH_BASE_ADDR     0x40022000UL
#define FLASH_KEYR          (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x08))
#define FLASH_SR            (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x10))
#define FLASH_CR            (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x14))

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL
#define FLASH_SR_BSY        (1u << 16)
#define FLASH_SR_ERRMASK    (0x3FA8u)   /* all error bits L4 RM Table 3 */
#define FLASH_CR_PG         (1u << 0)
#define FLASH_CR_PER        (1u << 1)
#define FLASH_CR_MER1       (1u << 2)
#define FLASH_CR_PNB_SHIFT  3
#define FLASH_CR_STRT       (1u << 16)
#define FLASH_CR_LOCK       (1u << 31)

#elif defined(NVLOG_STM32_FLASH_H7)

#define FLASH_BASE_ADDR     0x52002000UL  /* Bank 1 */
#define FLASH_KEYR          (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x04))
#define FLASH_SR            (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x10))
#define FLASH_CR            (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x0C))
#define FLASH_CCR           (*(volatile uint32_t *)(FLASH_BASE_ADDR + 0x14))

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL
#define FLASH_SR_QW         (1u << 2)   /* write queue wait */
#define FLASH_SR_BSY        (1u << 0)
#define FLASH_SR_ERRMASK    (0x07EEu)
#define FLASH_CR_PG         (1u << 1)
#define FLASH_CR_SER        (1u << 2)
#define FLASH_CR_SNB_SHIFT  8
#define FLASH_CR_START      (1u << 7)
#define FLASH_CR_LOCK       (1u << 0)

#endif /* family */

/* ════════════════════════════════════════════════════════════════
 * INTERNAL HELPERS
 * ════════════════════════════════════════════════════════════════ */

static int flash_wait_ready(void)
{
    /* spin until not busy — in real code: add watchdog kick or timeout */
    uint32_t timeout = 0xFFFFFFUL;
    while (timeout--) {
#if defined(NVLOG_STM32_FLASH_H7)
        if (!(FLASH_SR & (FLASH_SR_BSY | FLASH_SR_QW)))
            break;
#else
        if (!(FLASH_SR & FLASH_SR_BSY))
            break;
#endif
    }
    if (FLASH_SR & FLASH_SR_ERRMASK) {
        /* clear error flags */
        FLASH_SR = FLASH_SR_ERRMASK;
        return -1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * UNLOCK / LOCK
 * ════════════════════════════════════════════════════════════════ */

nvlog_status_t nvlog_stm32_flash_unlock(void)
{
    if (flash_wait_ready() != 0) return NVLOG_ERR_IO;
    /* write magic key sequence */
    FLASH_KEYR = FLASH_KEY1;
    FLASH_KEYR = FLASH_KEY2;
    /* verify LOCK bit cleared */
    if (FLASH_CR & FLASH_CR_LOCK) return NVLOG_ERR_IO;
    return NVLOG_OK;
}

void nvlog_stm32_flash_lock(void)
{
    FLASH_CR |= FLASH_CR_LOCK;
}

/* ════════════════════════════════════════════════════════════════
 * READ
 * (internal flash is memory-mapped — just memcpy from address)
 * ════════════════════════════════════════════════════════════════ */

static int stm32_flash_read(uint32_t addr, void *buf, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t abs_addr = ctx->base_addr + addr;
    memcpy(buf, (const void *)abs_addr, len);
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * WRITE (program)
 * ════════════════════════════════════════════════════════════════ */

#if defined(NVLOG_STM32_FLASH_F4)

/**
 * F4: 32-bit (PSIZE=10) program.
 * Writes must be 4-byte aligned. We pad the last chunk if needed.
 */
static int stm32_flash_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx   = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t                 abs   = ctx->base_addr + addr;
    const uint8_t           *src   = (const uint8_t *)buf;
    uint32_t                 remaining = len;

    if (flash_wait_ready() != 0) return -1;
    FLASH_CR = FLASH_CR_PG | FLASH_CR_PSIZE_X32;

    while (remaining > 0) {
        uint32_t word = 0xFFFFFFFFUL;
        uint32_t n    = remaining < 4u ? remaining : 4u;
        for (uint32_t i = 0; i < n; i++)
            ((uint8_t *)&word)[i] = src[i];

        *(volatile uint32_t *)abs = word;
        if (flash_wait_ready() != 0) { FLASH_CR = 0; return -1; }

        abs       += 4;
        src       += n;
        remaining -= n;
    }

    FLASH_CR = 0;
    return 0;
}

#elif defined(NVLOG_STM32_FLASH_L4)

/**
 * L4: double-word (64-bit) program.
 * Must write exactly 8 bytes at a time, 8-byte aligned.
 * Pad with 0xFF for partial writes.
 */
static int stm32_flash_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t abs  = ctx->base_addr + addr;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = len;

    if (abs % 8 != 0) return -1;  /* must be 8-byte aligned */
    if (flash_wait_ready() != 0) return -1;

    FLASH_CR |= FLASH_CR_PG;

    while (remaining > 0) {
        uint32_t lo = 0xFFFFFFFFUL, hi = 0xFFFFFFFFUL;
        uint32_t n  = remaining < 8u ? remaining : 8u;

        for (uint32_t i = 0; i < n && i < 4; i++)
            ((uint8_t *)&lo)[i] = src[i];
        for (uint32_t i = 4; i < n; i++)
            ((uint8_t *)&hi)[i - 4] = src[i];

        *(volatile uint32_t *)abs       = lo;
        *(volatile uint32_t *)(abs + 4) = hi;

        if (flash_wait_ready() != 0) { FLASH_CR &= ~FLASH_CR_PG; return -1; }

        abs       += 8;
        src       += n;
        remaining -= n;
    }

    FLASH_CR &= ~FLASH_CR_PG;
    return 0;
}

#elif defined(NVLOG_STM32_FLASH_H7)

/**
 * H7: 256-bit (32-byte) flash word program.
 * Must write exactly 32 bytes at a time, 32-byte aligned.
 */
static int stm32_flash_write(uint32_t addr, const void *buf, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t abs  = ctx->base_addr + addr;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = len;

    if (abs % 32 != 0) return -1;
    if (flash_wait_ready() != 0) return -1;

    FLASH_CR |= FLASH_CR_PG;

    while (remaining > 0) {
        uint8_t  word[32];
        uint32_t n = remaining < 32u ? remaining : 32u;

        for (uint32_t i = 0; i < 32; i++)
            word[i] = (i < n) ? src[i] : 0xFF;

        /* write 8 x 32-bit words */
        for (int i = 0; i < 8; i++) {
            uint32_t v;
            memcpy(&v, word + i * 4, 4);
            *(volatile uint32_t *)(abs + i * 4) = v;
        }

        if (flash_wait_ready() != 0) { FLASH_CR &= ~FLASH_CR_PG; return -1; }

        abs       += 32;
        src       += n;
        remaining -= n;
    }

    FLASH_CR &= ~FLASH_CR_PG;
    return 0;
}

#endif /* write family */

/* ════════════════════════════════════════════════════════════════
 * ERASE (sector/page)
 * ════════════════════════════════════════════════════════════════ */

#if defined(NVLOG_STM32_FLASH_F4)

/**
 * F4 sector erase. Sector numbers are non-uniform:
 *   Sector 0-3: 16KB each  (0x08000000 - 0x0800FFFF)
 *   Sector 4:   64KB       (0x08010000 - 0x0801FFFF)
 *   Sector 5+:  128KB each
 *
 * addr must be the start of a sector; len must equal the sector size.
 * For nvlog, use a region aligned to 128KB sectors for simplicity.
 *
 * This implementation assumes all nvlog sectors are 128KB (sectors 5+).
 * Adjust sector_number calculation for your device layout.
 */
static int stm32_flash_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t abs = ctx->base_addr + addr;

    /* compute sector number from absolute address (F407 layout) */
    uint32_t offset_from_start = abs - 0x08000000UL;
    uint32_t sector_num;

    if (offset_from_start < 4 * 16 * 1024)
        sector_num = offset_from_start / (16 * 1024);
    else if (offset_from_start < 4 * 16 * 1024 + 64 * 1024)
        sector_num = 4;
    else
        sector_num = 5 + (offset_from_start - 4 * 16 * 1024 - 64 * 1024) / (128 * 1024);

    (void)len;  /* len must equal sector size — caller's responsibility */

    if (flash_wait_ready() != 0) return -1;

    FLASH_CR = FLASH_CR_SER | FLASH_CR_PSIZE_X32 |
               ((sector_num & 0xF) << FLASH_CR_SNB_SHIFT);
    FLASH_CR |= FLASH_CR_STRT;

    if (flash_wait_ready() != 0) { FLASH_CR = 0; return -1; }

    FLASH_CR = 0;
    return 0;
}

#elif defined(NVLOG_STM32_FLASH_L4)

/**
 * L4 page erase. Page size = 2KB. Pages are numbered 0..255.
 * addr is relative to nvlog base (not absolute flash addr).
 */
static int stm32_flash_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t abs      = ctx->base_addr + addr;
    uint32_t page_num = (abs - 0x08000000UL) / 2048u;
    uint32_t pages    = len / 2048u;

    for (uint32_t p = 0; p < pages; p++) {
        if (flash_wait_ready() != 0) return -1;

        FLASH_CR = FLASH_CR_PER | ((page_num + p) << FLASH_CR_PNB_SHIFT);
        FLASH_CR |= FLASH_CR_STRT;

        if (flash_wait_ready() != 0) { FLASH_CR = 0; return -1; }
    }
    FLASH_CR = 0;
    return 0;
}

#elif defined(NVLOG_STM32_FLASH_H7)

/**
 * H7 sector erase. Sector size = 128KB.
 * Sector 0 starts at 0x08000000.
 */
static int stm32_flash_erase(uint32_t addr, uint32_t len, void *user)
{
    nvlog_stm32_flash_ctx_t *ctx    = (nvlog_stm32_flash_ctx_t *)user;
    uint32_t                 abs    = ctx->base_addr + addr;
    uint32_t                 sector = (abs - 0x08000000UL) / (128 * 1024u);
    uint32_t                 count  = len / (128 * 1024u);

    for (uint32_t s = 0; s < count; s++) {
        if (flash_wait_ready() != 0) return -1;

        FLASH_CR = FLASH_CR_SER | ((sector + s) << FLASH_CR_SNB_SHIFT);
        FLASH_CR |= FLASH_CR_START;

        if (flash_wait_ready() != 0) { FLASH_CR = 0; return -1; }
    }
    FLASH_CR = 0;
    return 0;
}

#endif /* erase family */

/* ════════════════════════════════════════════════════════════════
 * INIT
 * ════════════════════════════════════════════════════════════════ */

nvlog_status_t nvlog_stm32_flash_init(nvlog_stm32_flash_ctx_t *ctx,
                                       nvlog_hal_flash_t       *flash_out,
                                       uint32_t                 base_addr,
                                       uint32_t                 region_size)
{
    if (!ctx || !flash_out)               return NVLOG_ERR_PARAM;
    if (base_addr == 0 || region_size == 0) return NVLOG_ERR_PARAM;
    if (region_size % NVLOG_STM32_ERASE_MIN != 0) return NVLOG_ERR_PARAM;

    ctx->base_addr   = base_addr;
    ctx->region_size = region_size;

    flash_out->base.read  = stm32_flash_read;
    flash_out->base.write = stm32_flash_write;
    flash_out->base.user  = ctx;

    flash_out->erase      = stm32_flash_erase;
    flash_out->erase_size = NVLOG_STM32_ERASE_MIN;
    flash_out->prog_size  = NVLOG_STM32_PROG_SIZE;
    flash_out->user       = ctx;

    return NVLOG_OK;
}
