/**
 * nvlog — persistent record buffer for byte-writable and erase-before-write media.
 *
 * Public API version: 1.0.5.
 *
 * The implementation uses explicit little-endian wire encoding, committed
 * superblocks, committed DATA/WRAP/PADDING records, and wrap-aware recovery.
 * Power-loss behavior is backend- and media-specific; do not assume generic
 * flash safety from the core API alone.
 */

#ifndef NVLOG_H
#define NVLOG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- version --------------------------------------------------- */

#define NVLOG_VERSION_MAJOR 1
#define NVLOG_VERSION_MINOR 0
#define NVLOG_VERSION_PATCH 5

/* --- limits ---------------------------------------------------- */

#define NVLOG_MEDIA_MAGIC         0x4E564C47UL  /* "NVLG" */
#define NVLOG_MEDIA_VERSION       0x0005u
#define NVLOG_REGION_MAGIC        NVLOG_MEDIA_MAGIC
#define NVLOG_RECORD_MAGIC        0x4Eu
#define NVLOG_RECORD_VERSION      0x05u
#define NVLOG_HEADER_SIZE         32u
#define NVLOG_SUPERBLOCK_SIZE     64u
#define NVLOG_SUPERBLOCK_COUNT    2u
#define NVLOG_MEDIA_HEADER_SIZE   (NVLOG_SUPERBLOCK_SIZE * NVLOG_SUPERBLOCK_COUNT)
#define NVLOG_RECORD_HEADER_SIZE  32u
#define NVLOG_RECORD_CRC_SIZE     4u
#define NVLOG_RECORD_COMMIT_SIZE  1u
#define NVLOG_MAX_PROGRAM_UNIT   32u
#define NVLOG_RECORD_OVERHEAD     (NVLOG_RECORD_HEADER_SIZE + NVLOG_RECORD_CRC_SIZE + NVLOG_RECORD_COMMIT_SIZE)
#define NVLOG_MAX_PAYLOAD         65535u
#define NVLOG_REGION_HEADER_SIZE  NVLOG_MEDIA_HEADER_SIZE

typedef enum {
    NVLOG_RECORD_TYPE_DATA      = 0x01u,
    NVLOG_RECORD_TYPE_WRAP      = 0x02u,
    NVLOG_RECORD_TYPE_PADDING   = 0x03u,
} nvlog_record_type_t;

typedef enum {
    NVLOG_MEDIA_CLASS_BYTE_WRITABLE = 0u,
    NVLOG_MEDIA_CLASS_ERASE_BEFORE_WRITE = 1u,
} nvlog_media_class_t;

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
    NVLOG_ERR_UNSUPPORTED = -10, /* unsupported mode/version/media combination */
    NVLOG_ERR_END         = -11, /* clean erased end of log */
    NVLOG_ERR_INCOMPLETE  = -12, /* incomplete tail record */
    NVLOG_ERR_VERSION     = -13, /* unsupported record or media version */
    NVLOG_ERR_TYPE        = -14, /* unsupported record type */
    NVLOG_ERR_FLAGS       = -15, /* unsupported record flags */
    NVLOG_ERR_RESERVED    = -16, /* non-zero reserved fields */
    NVLOG_ERR_MODE_MISMATCH = -17, /* record mode does not match mount mode */
    NVLOG_ERR_GENERATION_MISMATCH = -18, /* record generation does not match mount generation */
    NVLOG_ERR_SIZE_MISMATCH = -19, /* record length fields disagree */
    NVLOG_ERR_MEDIA_MISMATCH = -20, /* record media/state disagrees with mount */
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

/* --- context --------------------------------------------------- */

typedef struct {
    nvlog_hal_t   hal;
    uint32_t      region_size;   /* total NVM bytes available */
    uint32_t      write_ptr;     /* byte offset of next write */
    uint32_t      next_seq;      /* next sequence number to assign */
    uint32_t      mutation;      /* increments on mount/format/append */
    uint32_t      generation;    /* active media generation */
    uint32_t      metadata_seq;   /* superblock publication counter */
    uint32_t      geometry_key;   /* packed erase/program geometry identity */
    uint32_t      session_id;     /* incremented on each successful mount/format */
    uint8_t       media_class;    /* NVLOG_MEDIA_CLASS_* */
    uint8_t       program_unit;   /* physical program unit in bytes */
    uint8_t       erased_value;    /* erased byte value, typically 0xFF */
    uint8_t       mounted;       /* 1 after successful nvlog_mount() */

    /* ring mode fields */
    nvlog_mode_t  mode;          /* NVLOG_MODE_LINEAR or NVLOG_MODE_RING */
    uint32_t      tail_ptr;      /* oldest record offset (ring mode only) */
    uint8_t       ring_full;     /* 1 when ring has wrapped at least once */
    uint32_t      record_count;  /* valid records currently in ring (ring mode only) */
    uint32_t      used_bytes;    /* ring: committed live bytes */
    uint32_t      free_bytes;    /* ring: committed free bytes */
    uint32_t      padding_bytes; /* ring: explicit padding / wrap bytes */
    uint32_t      reserve_bytes; /* ring: physical reserve for atomic overwrite */
} nvlog_ctx_t;

/* --- record (returned by iterator) ---------------------------- */

typedef struct {
    uint32_t  seq;          /* sequence number */
    uint16_t  len;          /* payload length */
    uint32_t  offset;       /* NVM offset of this record (for debugging) */
    uint32_t  generation;   /* media generation observed at iteration time */
    uint32_t  session_id;   /* mount/session identity observed at iteration time */
    uint32_t  crc32;        /* payload CRC32 */
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
    uint32_t      snapshot_generation;
    uint32_t      snapshot_session_id;
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
                             size_t len);

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
                                   size_t buf_size);

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
 * Restores the latest committed ring state, including head/tail/write
 * positions, live counts, and next sequence.
 *
 * Returns NVLOG_ERR_CORRUPT for a corrupt committed ring image.
 * Returns NVLOG_ERR_UNSUPPORTED for a mismatched version or mode.
 */
nvlog_status_t nvlog_ring_mount(nvlog_ctx_t       *ctx,
                                  const nvlog_hal_t *hal,
                                  uint32_t           region_size);

/**
 * nvlog_ring_count() — number of valid records currently in the ring.
 */
uint32_t nvlog_ring_count(nvlog_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_H */
