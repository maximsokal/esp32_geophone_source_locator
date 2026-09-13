#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define RUNTIME_OBSERVABILITY_TOP_TASKS 10U


typedef struct
{
    const void *identity;
    uint64_t runtime_delta;
    unsigned cpu_tenths;

} runtime_observability_usage_t;


unsigned runtime_observability_percent_tenths(
    uint64_t numerator,
    uint64_t denominator
);


void runtime_observability_usage_insert(
    runtime_observability_usage_t top_tasks[RUNTIME_OBSERVABILITY_TOP_TASKS],
    size_t *top_count,
    const void *identity,
    uint64_t runtime_delta,
    unsigned cpu_tenths
);


bool runtime_observability_status_storage_allocate(
    size_t bytes,
    void **previous,
    void **current,
    bool *used_internal
);
