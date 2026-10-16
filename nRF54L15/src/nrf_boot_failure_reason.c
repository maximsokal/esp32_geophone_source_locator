#include "nrf_boot_failure_reason.h"

#include <errno.h>

nrf_boot_failure_reason_t nrf_boot_failure_reason_from_errno(int error_code)
{
    if (error_code == -ENOMEM
#ifdef ENOBUFS
        || error_code == -ENOBUFS
#endif
#ifdef ENOSPC
        || error_code == -ENOSPC
#endif
    ) {
        return NRF_BOOT_FAILURE_RESOURCE_EXHAUSTED;
    }
    return NRF_BOOT_FAILURE_BOOT_FAILED;
}
