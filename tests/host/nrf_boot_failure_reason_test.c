#include <assert.h>
#include <errno.h>

#include "nrf_boot_failure_reason.h"

static void test_resource_exhaustion_maps_to_a_stable_reason(void)
{
    assert(nrf_boot_failure_reason_from_errno(-ENOMEM) == NRF_BOOT_FAILURE_RESOURCE_EXHAUSTED);
    assert(nrf_boot_failure_reason_from_errno(-ENOBUFS) == NRF_BOOT_FAILURE_RESOURCE_EXHAUSTED);
    assert(nrf_boot_failure_reason_from_errno(-ENOSPC) == NRF_BOOT_FAILURE_RESOURCE_EXHAUSTED);
    assert(nrf_boot_failure_reason_from_errno(-EIO) == NRF_BOOT_FAILURE_BOOT_FAILED);
    assert(nrf_boot_failure_reason_from_errno(0) == NRF_BOOT_FAILURE_BOOT_FAILED);
}

int main(void)
{
    test_resource_exhaustion_maps_to_a_stable_reason();
    return 0;
}
