/**
 * nvlog — Tiny persistent record buffer for MCUs
 *
 * Supports two modes:
 *
 *   LINEAR: append-only, stops when full.
 *   RING:   circular, overwrites oldest records when full.
 *
 * Memory layout:
 *
 *   LINEAR:  [REGION_HEADER][RECORD_0][RECORD_1]...[RECORD_N][free...]
 *   RING:    [REGION_HEADER][...records wrap around the region...]
 *
 * Record layout (both modes):
 *
 *   [MAGIC:1][FLAGS:1][LEN:2][SEQ:4][PAYLOAD:N][CRC32:4]
 *
 *   MAGIC   - 0x4E ('N'), fast scan anchor
 *   FLAGS   - mode: 0x00=linear, 0x01=ring
 *   LEN     - payload length (uint16)
 *   SEQ     - monotonic, local to this format() call; resets on nvlog_format/ring_format
 *   PAYLOAD - user data
 *   CRC32   - over MAGIC+FLAGS+LEN+SEQ+PAYLOAD (written last = commit)
 *
 * Ring mode recovery (2-pass, O(n) scan, no heap):
 *   Pass 1: scan all records → find max SEQ → derive write_ptr
 *   Pass 2: scan forward from write_ptr (wrapping) → find tail_ptr
 *
 * Power-loss guarantee (both modes):
 *   CRC is written last. Interrupted records have no CRC → invisible
 *   on next mount. Previously committed records are always intact.
 */

#ifndef NVLOG_H
#define NVLOG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- version --------------------------------------------------- */

#define NVLOG_VERSION_MAJOR 0
#define NVLOG_VERSION_MINOR 4
#define NVLOG_VERSION_PATCH 0

/* --- limits ---------------------------------------------------- */

#define NVLOG_RECORD_MAGIC        0x4Eu
#define NVLOG_REGION_MAGIC        0x4E564C47UL  /* "NVLG" */
#define NVLOG_HEADER_SIZE         8u            /* MAGIC+FLAGS+LEN+SEQ */
#define NVLOG_CRC_SIZE            4u
#define NVLOG_RECORD_OVERHEAD     (NVLOG_HEADER_SIZE + NVLOG_CRC_SIZE)
#define NVLOG_MAX_PAYLOAD         65535u

/* FLAGS field values (byte 1 of record header) */
#define NVLOG_FLAGS_LINEAR        0x00u
#define NVLOG_FLAGS_RING          0x01u

/* --- mode ------------------------------------------------------ */

typedef enum {
    NVLOG_MODE_LINEAR = 0,   /* default: append until full */
    NVLOG_MODE_RING   = 1,   /* circular: overwrites oldest records */
} nvlog_mode_t;

/* --- status codes ---------------------------------------------- */

typedef enum {
    NVLOG_OK              =  0,  /* success */
    NVLOG_ERR_PARAM       = -1,  /* bad argument */
    NVLOG_ERR_FULL        = -2,  /* no space left (linear mode) */
    NVLOG_ERR_IO          = -3,  /* HAL read/write failed */
    NVLOG_ERR_CORRUPT     = -4,  /* CRC mismatch or bad magic */
    NVLOG_ERR_NO_DATA     = -5,  /* iterator exhausted / ring is empty */
    NVLOG_ERR_TOO_LARGE   = -6,  /* payload exceeds NVLOG_MAX_PAYLOAD */
    NVLOG_ERR_NOT_MOUNTED = -7,  /* nvlog_mount() not called */
    NVLOG_ERR_STALE       = -8,  /* iterator or descriptor snapshot invalid */
    NVLOG_ERR_BOUNDS      = -9,  /* checked arithmetic or range validation failed */
} nvlog_status_t;

/* --- HAL — byte-writable backends only ------------------------ */

/**
 * Backend HAL for byte-writable NVM.
 *
 * Supported media: FRAM, EEPROM, RAM (simulation/test)
 * NOT for: NOR flash, internal MCU flash (requires erase)
 *
 * All addresses are relative to the start of the nvlog region.
 * Implementor must ensure write is byte-granular and non-destructive
 * (i.e., no erase required before write).
 *
 * Returns 0 on success, negative on error.
 */
typedef struct {
    int (*read) (uint32_t addr, void *buf, uint32_t len, void *user);
    int (*write)(uint32_t addr, const void *buf, uint32_t len, void *user);
    void *user;  /* passed back to read/write, e.g. device handle */
} nvlog_hal_t;

/* --- region header (written once at NVM offset 0) ------------- */

#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      /* NVLOG_REGION_MAGIC */
    uint32_t region_size;
} nvlog_region_header_t;
#pragma pack(pop)
#else
typedef struct __attribute__((packed)) {
    uint32_t magic;      /* NVLOG_REGION_MAGIC */
    uint32_t region_size;
} nvlog_region_header_t;
#endif

#define NVLOG_REGION_HEADER_SIZE sizeof(nvlog_region_header_t)

/* --- context --------------------------------------------------- */

typedef struct {
    nvlog_hal_t   hal;
    uint32_t      region_size;   /* total NVM bytes available */
    uint32_t      write_ptr;     /* byte offset of next write */
    uint32_t      next_seq;      /* next sequence number to assign */
    uint32_t      mutation;      /* increments on mount/format/append */
    uint8_t       mounted;       /* 1 after successful nvlog_mount() */

    /* ring mode fields */
    nvlog_mode_t  mode;          /* NVLOG_MODE_LINEAR or NVLOG_MODE_RING */
    uint32_t      tail_ptr;      /* oldest record offset (ring mode only) */
    uint8_t       ring_full;     /* 1 when ring has wrapped at least once */
    uint32_t      record_count;  /* valid records currently in ring (ring mode only) */
} nvlog_ctx_t;

/* --- record (returned by iterator) ---------------------------- */

typedef struct {
    uint32_t  seq;          /* sequence number */
    uint16_t  len;          /* payload length */
    uint32_t  offset;       /* NVM offset of this record (for debugging) */
    /* payload is NOT copied here — use nvlog_read_payload() */
} nvlog_record_t;

/* --- iterator -------------------------------------------------- */

typedef struct {
    nvlog_ctx_t  *ctx;
    uint32_t      cursor;      /* current NVM offset */
    uint32_t      count;       /* records yielded so far */
    /* ring mode fields */
    uint32_t      stop_ptr;    /* where to stop (write_ptr in ring mode) */
    uint8_t       wrapped;     /* 1 after cursor has wrapped around */
    uint32_t      snapshot_mutation; /* ctx mutation observed at init */
} nvlog_iter_t;

/* --- API ------------------------------------------------------- */

/**
 * nvlog_format() — erase and initialise a region.
 *
 * Call once when setting up a fresh NVM region.
 * Writes region header, zeros write pointer and sequence.
 * Safe to call again to wipe all data.
 */
nvlog_status_t nvlog_format(nvlog_ctx_t *ctx,
                             const nvlog_hal_t *hal,
                             uint32_t region_size);

/**
 * nvlog_mount() — mount an existing region.
 *
 * Scans NVM, finds last valid record, restores write pointer
 * and sequence number. Must be called before append/iter.
 *
 * On first boot with fresh NVM: call nvlog_format() first.
 * On any subsequent boot: call nvlog_mount().
 */
nvlog_status_t nvlog_mount(nvlog_ctx_t *ctx,
                            const nvlog_hal_t *hal,
                            uint32_t region_size);

/**
 * nvlog_append() — write one record.
 *
 * Atomically (from recovery perspective) appends payload.
 * CRC32 is computed and written as the final commit step.
 * If power is lost before CRC write, record is invisible on next mount.
 *
 * Returns NVLOG_ERR_FULL if not enough space remains.
 */
nvlog_status_t nvlog_append(nvlog_ctx_t *ctx,
                             const void *payload,
                             uint16_t len);

/**
 * nvlog_iter_init() — start iterating from oldest record.
 */
nvlog_status_t nvlog_iter_init(nvlog_iter_t *it, nvlog_ctx_t *ctx);

/**
 * nvlog_iter_next() — advance iterator, fill record header.
 *
 * Returns NVLOG_ERR_NO_DATA when all records consumed.
 * Skips corrupt records (bad magic, bad CRC) silently —
 * they are treated as end-of-log.
 */
nvlog_status_t nvlog_iter_next(nvlog_iter_t *it, nvlog_record_t *out);

/**
 * nvlog_read_payload() — copy payload of a record into buf.
 *
 * buf must be at least record->len bytes.
 * Typically called right after nvlog_iter_next().
 */
nvlog_status_t nvlog_read_payload(nvlog_ctx_t *ctx,
                                   const nvlog_record_t *record,
                                   void *buf,
                                   uint16_t buf_size);

/**
 * nvlog_stats() — current usage counters.
 */
typedef struct {
    uint32_t used_bytes;    /* bytes consumed (headers + payloads) */
    uint32_t free_bytes;    /* bytes available */
    uint32_t record_count;  /* valid records written */
    uint32_t next_seq;      /* next sequence number */
} nvlog_stats_t;

nvlog_status_t nvlog_stats(nvlog_ctx_t *ctx, nvlog_stats_t *out);

/* --- ring mode API --------------------------------------------- */

/**
 * nvlog_ring_format() — initialise a region for ring (circular) mode.
 *
 * Behaves like nvlog_format() but sets mode = NVLOG_MODE_RING.
 * Subsequent appends will wrap around and overwrite oldest records
 * once the region is full.
 *
 * Byte-writable backends only (FRAM, EEPROM, RAM).
 * Do NOT use with NOR flash or STM32 internal flash.
 *
 * Minimum region_size: 2 * NVLOG_RECORD_OVERHEAD + 2 (two minimal records).
 */
nvlog_status_t nvlog_ring_format(nvlog_ctx_t       *ctx,
                                  const nvlog_hal_t *hal,
                                  uint32_t           region_size);

/**
 * nvlog_ring_mount() — recover a ring-mode region after reset.
 *
 * Two-pass recovery:
 *   Pass 1: scan all valid records → find highest SEQ → derive write_ptr.
 *   Pass 2: scan forward from write_ptr (wrapping) → find tail_ptr.
 *
 * If region header is missing/corrupt: returns NVLOG_ERR_CORRUPT.
 * If region contains zero valid records: returns NVLOG_OK with empty ring.
 *
 * Note: SEQ numbers are local to the current format session.
 * After nvlog_ring_format(), SEQ starts at 0 and increments forever.
 * After nvlog_ring_mount(), SEQ continues from the last committed record.
 * SEQ is NOT globally unique across format() calls.
 */
nvlog_status_t nvlog_ring_mount(nvlog_ctx_t       *ctx,
                                 const nvlog_hal_t *hal,
                                 uint32_t           region_size);

/**
 * nvlog_ring_count() — number of valid records currently in the ring.
 *
 * O(n) scan. For large rings, cache this value in your application.
 */
uint32_t nvlog_ring_count(nvlog_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_H */
