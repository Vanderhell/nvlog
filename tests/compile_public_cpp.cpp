extern "C" {
#include "../include/nvlog.h"
#include "../include/nvlog_hal_flash.h"
}

static_assert(sizeof(nvlog_ctx_t) > 0, "nvlog_ctx_t must be complete");
static_assert(sizeof(nvlog_iter_t) > 0, "nvlog_iter_t must be complete");
static_assert(sizeof(nvlog_record_t) > 0, "nvlog_record_t must be complete");
static_assert(sizeof(nvlog_hal_flash_t) > 0, "nvlog_hal_flash_t must be complete");

int main()
{
    return 0;
}
