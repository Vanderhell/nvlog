#ifndef NVLOG_ESPIDF_PARTITION_H
#define NVLOG_ESPIDF_PARTITION_H

#include "nvlog_hal_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_partition_t esp_partition_t;

typedef struct {
    const esp_partition_t *partition;
    uint32_t capacity;
    uint32_t erase_size;
    uint32_t program_size;
} nvlog_espidf_partition_ctx_t;

nvlog_status_t nvlog_espidf_partition_init(nvlog_espidf_partition_ctx_t *ctx,
                                           nvlog_hal_flash_t *flash_out,
                                           const esp_partition_t *partition,
                                           uint32_t capacity,
                                           uint32_t erase_size,
                                           uint32_t program_size);

#ifdef __cplusplus
}
#endif

#endif /* NVLOG_ESPIDF_PARTITION_H */
