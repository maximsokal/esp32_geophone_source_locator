#include "runtime_observability_policy.h"

#include <limits.h>

#include "esp_heap_caps.h"


unsigned runtime_observability_percent_tenths(
    uint64_t numerator,
    uint64_t denominator
)
{
    if (denominator == 0U) {
        return 0U;
    }

    const uint64_t scaled =
        numerator > UINT64_MAX / 1000U
            ? UINT64_MAX
            : numerator * 1000U;

    const uint64_t result = scaled / denominator;
    return result > 1000U ? 1000U : (unsigned)result;
}


void runtime_observability_usage_insert(
    runtime_observability_usage_t top_tasks[RUNTIME_OBSERVABILITY_TOP_TASKS],
    size_t *top_count,
    const void *identity,
    uint64_t runtime_delta,
    unsigned cpu_tenths
)
{
    if (top_tasks == NULL || top_count == NULL || identity == NULL) {
        return;
    }

    if (
        *top_count >= RUNTIME_OBSERVABILITY_TOP_TASKS &&
        runtime_delta <=
            top_tasks[RUNTIME_OBSERVABILITY_TOP_TASKS - 1U].runtime_delta
    ) {
        return;
    }

    size_t position =
        *top_count < RUNTIME_OBSERVABILITY_TOP_TASKS
            ? *top_count
            : RUNTIME_OBSERVABILITY_TOP_TASKS - 1U;

    while (
        position > 0U &&
        top_tasks[position - 1U].runtime_delta < runtime_delta
    ) {
        top_tasks[position] = top_tasks[position - 1U];
        --position;
    }

    top_tasks[position].identity = identity;
    top_tasks[position].runtime_delta = runtime_delta;
    top_tasks[position].cpu_tenths = cpu_tenths;

    if (*top_count < RUNTIME_OBSERVABILITY_TOP_TASKS) {
        ++(*top_count);
    }
}


bool runtime_observability_status_storage_allocate(
    size_t bytes,
    void **previous,
    void **current,
    bool *used_internal
)
{
    if (
        bytes == 0U ||
        previous == NULL ||
        current == NULL ||
        used_internal == NULL
    ) {
        return false;
    }

    *previous = NULL;
    *current = NULL;
    *used_internal = false;

    void *new_previous = heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    void *new_current = heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (new_previous != NULL && new_current != NULL) {
        *previous = new_previous;
        *current = new_current;
        return true;
    }

    heap_caps_free(new_previous);
    heap_caps_free(new_current);

    new_previous = heap_caps_malloc(
        bytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    new_current = heap_caps_malloc(
        bytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (new_previous == NULL || new_current == NULL) {
        heap_caps_free(new_previous);
        heap_caps_free(new_current);
        return false;
    }

    *previous = new_previous;
    *current = new_current;
    *used_internal = true;
    return true;
}
