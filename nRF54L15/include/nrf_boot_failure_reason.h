#ifndef NRF_BOOT_FAILURE_REASON_H
#define NRF_BOOT_FAILURE_REASON_H

typedef enum {
    NRF_BOOT_FAILURE_RESOURCE_EXHAUSTED = 1,
    NRF_BOOT_FAILURE_BOOT_FAILED = 2,
} nrf_boot_failure_reason_t;

nrf_boot_failure_reason_t nrf_boot_failure_reason_from_errno(int error_code);

#endif
