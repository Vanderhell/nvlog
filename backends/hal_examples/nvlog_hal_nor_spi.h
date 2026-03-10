/**
 * nvlog_hal_nor_spi.h — Generic SPI NOR flash HAL (W25Qxx / GD25Qxx / MX25Lxx)
 *
 * Compatible devices (JEDEC SPI NOR standard):
 *   Winbond  W25Q16 / W25Q32 / W25Q64 / W25Q128 / W25Q256
 *   GigaDevice GD25Q32 / GD25Q64 / GD25Q128
 *   Macronix MX25L3206 / MX25L6406 / MX25L12806
 *   ISSI     IS25LP032 / IS25LP064
 *
 * Flash geometry (all devices above):
 *   Sector erase:  4096 bytes  (4KB sectors)
 *   Page program:  256  bytes  (page boundary must not be crossed)
 *   Write enable:  required before every write and erase
 *   Busy polling:  RDSR bit 0 (WIP — Write In Progress)
 *
 * Address modes:
 *   3-byte (24-bit): devices ≤ 128Mbit (≤ 16MB)  — addr_bytes = 3
 *   4-byte (32-bit): devices > 128Mbit (W25Q256+) — addr_bytes = 4
 *
 * Opcodes used:
 *   0x06 WREN   Write Enable
 *   0x05 RDSR   Read Status Register (bit 0 = WIP)
 *   0x03 READ   Read Data (no dummy byte, up to 50MHz)
 *   0x02 PP     Page Program (256B)
 *   0x20 SE     Sector Erase (4KB)
 *   0xC7 CE     Chip Erase (not used by nvlog — too dangerous)
 *
 * Timing:
 *   Page program:    typ 0.7ms, max 3ms
 *   Sector erase:    typ 45ms,  max 400ms
 *   WIP poll period: 1ms recommended
 *
 * MCU glue required:
 *   cs_set(assert, user)
 *   spi_transfer(tx, rx, len, user)  — full-duplex, NULL = don't care
 *   delay_ms(ms, user)               — for WIP polling backoff
 */

#ifndef NVLOG_HAL_NOR_SPI_H
#define NVLOG_HAL_NOR_SPI_H

#include "nvlog_hal_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── SPI NOR opcodes ────────────────────────────────────────── */

#define NOR_OP_WREN    0x06u
#define NOR_OP_RDSR    0x05u
#define NOR_OP_READ    0x03u
#define NOR_OP_PP      0x02u   /* Page Program */
#define NOR_OP_SE      0x20u   /* Sector Erase 4KB */
#define NOR_OP_BE32    0x52u   /* Block Erase 32KB */
#define NOR_OP_BE64    0xD8u   /* Block Erase 64KB */
#define NOR_OP_RDID    0x9Fu   /* Read JEDEC ID */

#define NOR_SR_WIP     0x01u   /* Write In Progress bit */

#define NOR_SECTOR_SIZE  4096u
#define NOR_PAGE_SIZE     256u

/* ─── MCU glue ───────────────────────────────────────────────── */

typedef struct {
    void (*cs_set)    (int assert, void *user);
    int  (*transfer)  (const uint8_t *tx, uint8_t *rx, uint32_t len, void *user);
    void (*delay_ms)  (uint32_t ms, void *user);   /* NULL = spin */
    void *user;

    uint8_t  addr_bytes;      /* 3 for ≤16MB, 4 for W25Q256 and above */
    uint32_t busy_timeout_ms; /* WIP poll timeout: 400ms safe for 4KB sector */
} nvlog_nor_spi_glue_t;

/* ─── HAL context ────────────────────────────────────────────── */

typedef struct {
    nvlog_nor_spi_glue_t glue;
} nvlog_nor_spi_ctx_t;

/* ─── init ───────────────────────────────────────────────────── */

/**
 * nvlog_nor_spi_init() — configure W25Qxx/compatible SPI NOR backend.
 *
 * Fills flash_out (which embeds a compatible nvlog_hal_t).
 * ctx must remain valid for the nvlog session lifetime.
 *
 * After init, use nvlog_flash_format() for first-time setup,
 * or nvlog_mount() for subsequent boots.
 */
nvlog_status_t nvlog_nor_spi_init(nvlog_nor_spi_ctx_t         *ctx,
                                   nvlog_hal_flash_t           *flash_out,
                                   const nvlog_nor_spi_glue_t  *glue);

/**
 * nvlog_nor_spi_read_jedec_id() — read 3-byte JEDEC manufacturer ID.
 * Useful for device identification and sanity check at startup.
 * id_out must point to a 3-byte buffer.
 */
nvlog_status_t nvlog_nor_spi_read_jedec_id(nvlog_nor_spi_ctx_t *ctx,
                                            uint8_t *id_out);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_HAL_NOR_SPI_H */
