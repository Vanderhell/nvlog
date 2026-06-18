#include "../include/nvlog.h"
#include "../include/nvlog_hal_flash.h"
#include "../backends/hal_examples/nvlog_espidf_partition.h"

#include <stdio.h>

typedef char nvlog_ctx_size_check[(sizeof(nvlog_ctx_t) > 0) ? 1 : -1];
typedef char nvlog_iter_size_check[(sizeof(nvlog_iter_t) > 0) ? 1 : -1];
typedef char nvlog_record_size_check[(sizeof(nvlog_record_t) > 0) ? 1 : -1];
typedef char nvlog_flash_size_check[(sizeof(nvlog_hal_flash_t) > 0) ? 1 : -1];

int main(void)
{
    printf("sizeof(nvlog_ctx_t)=%zu\n", sizeof(nvlog_ctx_t));
    printf("sizeof(nvlog_iter_t)=%zu\n", sizeof(nvlog_iter_t));
    printf("sizeof(nvlog_record_t)=%zu\n", sizeof(nvlog_record_t));
    printf("sizeof(nvlog_hal_flash_t)=%zu\n", sizeof(nvlog_hal_flash_t));
    printf("sizeof(nvlog_espidf_partition_ctx_t)=%zu\n",
           sizeof(nvlog_espidf_partition_ctx_t));
    return 0;
}
