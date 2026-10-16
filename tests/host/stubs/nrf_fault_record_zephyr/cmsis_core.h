#ifndef CMSIS_CORE_H
#define CMSIS_CORE_H

#include <stdint.h>

typedef struct {
    uint32_t MMFAR;
    uint32_t CFSR;
} test_scb_t;

extern test_scb_t nrf_fault_test_scb;

#define SCB (&nrf_fault_test_scb)
#define __DSB() ((void)0)

#endif
