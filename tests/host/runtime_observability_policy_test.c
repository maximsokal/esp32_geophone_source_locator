#define _GNU_SOURCE 1
#define ESP_PLATFORM 1

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Compile the actual target implementation in this host translation unit so
 * these tests exercise the production static policy helpers rather than a
 * copied/reimplemented version of them. */
#include "../../components/runtime_observability/runtime_observability.c"


static int64_t s_now_us;
static bool s_fail_spiram;
static bool s_fail_internal;
static unsigned s_spiram_allocs;
static unsigned s_internal_allocs;
static unsigned s_fallback_logs;
static TaskStatus_t s_mock_task_statuses[4];
static UBaseType_t s_mock_task_count;
static unsigned s_runtime_task_info_logs;
static unsigned s_low_stack_warning_logs;
static bool s_saw_low_stack_511;
static bool s_saw_low_stack_0;
static char s_last_runtime_task_info[512];
static unsigned s_free_caps_internal_8bit;
static unsigned s_free_caps_internal_dma;
static unsigned s_free_caps_spiram;
static unsigned s_free_caps_spiram_dma;
static unsigned s_largest_caps_internal_8bit;
static unsigned s_largest_caps_internal_dma;
static unsigned s_largest_caps_spiram;
static unsigned s_largest_caps_spiram_dma;


static void reset_runtime_storage(void)
{
    heap_caps_free(s_previous_status);
    heap_caps_free(s_current_status);
    s_previous_status = NULL;
    s_current_status = NULL;
    s_status_capacity = 0U;
    s_previous_count = 0U;
    s_previous_snapshot_us = 0U;
    s_previous_idle_us[0] = 0U;
    s_previous_idle_us[1] = 0U;
    s_have_previous_snapshot = false;

    s_fail_spiram = false;
    s_fail_internal = false;
    s_spiram_allocs = 0U;
    s_internal_allocs = 0U;
    s_fallback_logs = 0U;
    s_mock_task_count = 0U;
    memset(s_mock_task_statuses, 0, sizeof(s_mock_task_statuses));
    s_runtime_task_info_logs = 0U;
    s_low_stack_warning_logs = 0U;
    s_saw_low_stack_511 = false;
    s_saw_low_stack_0 = false;
    s_last_runtime_task_info[0] = '\0';
    s_snapshot_mutex = NULL;
    s_free_caps_internal_8bit = 0U;
    s_free_caps_internal_dma = 0U;
    s_free_caps_spiram = 0U;
    s_free_caps_spiram_dma = 0U;
    s_largest_caps_internal_8bit = 0U;
    s_largest_caps_internal_dma = 0U;
    s_largest_caps_spiram = 0U;
    s_largest_caps_spiram_dma = 0U;
}


int64_t esp_timer_get_time(void)
{
    return s_now_us;
}


void fake_runtime_log(
    const char *level,
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    if (strstr(format, "RUNTIME_SNAPSHOT_STATUS_STORAGE fallback=internal") != NULL) {
        s_fallback_logs++;
    }

    va_list args;
    va_start(args, format);
    char message[512];
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (strcmp(level, "I") == 0 && strstr(message, "RUNTIME_TASK rank=") != NULL) {
        ++s_runtime_task_info_logs;
        (void)snprintf(
            s_last_runtime_task_info,
            sizeof(s_last_runtime_task_info),
            "%s",
            message
        );
    } else if (
        strcmp(level, "W") == 0 &&
        strstr(message, "RUNTIME_TASK_LOW_STACK name=") != NULL
    ) {
        ++s_low_stack_warning_logs;
        if (strcmp(
                message,
                "RUNTIME_TASK_LOW_STACK name=below stack_free=511"
            ) == 0) {
            s_saw_low_stack_511 = true;
        }
        if (strcmp(
                message,
                "RUNTIME_TASK_LOW_STACK name=zero stack_free=0"
            ) == 0) {
            s_saw_low_stack_0 = true;
        }
    }
}


void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if ((caps & MALLOC_CAP_SPIRAM) != 0U) {
        s_spiram_allocs++;
        if (s_fail_spiram) {
            return NULL;
        }
    } else if ((caps & MALLOC_CAP_INTERNAL) != 0U) {
        s_internal_allocs++;
        if (s_fail_internal) {
            return NULL;
        }
    }

    return malloc(size > 0U ? size : 1U);
}


void heap_caps_free(void *ptr)
{
    free(ptr);
}


static void record_caps(uint32_t caps, bool largest)
{
    unsigned *counter = NULL;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        counter = largest
            ? &s_largest_caps_internal_8bit
            : &s_free_caps_internal_8bit;
    } else if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) {
        counter = largest
            ? &s_largest_caps_internal_dma
            : &s_free_caps_internal_dma;
    } else if (caps == MALLOC_CAP_SPIRAM) {
        counter = largest
            ? &s_largest_caps_spiram
            : &s_free_caps_spiram;
    } else if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)) {
        counter = largest
            ? &s_largest_caps_spiram_dma
            : &s_free_caps_spiram_dma;
    }

    assert(counter != NULL);
    (*counter)++;
}


size_t heap_caps_get_free_size(uint32_t caps)
{
    record_caps(caps, false);
    return 1234U + caps;
}


size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    record_caps(caps, true);
    return 567U + caps;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    return heap_caps_get_free_size(caps);
}


UBaseType_t uxTaskGetNumberOfTasks(void)
{
    return s_mock_task_count;
}


UBaseType_t uxTaskGetSystemState(
    TaskStatus_t *task_status_array,
    UBaseType_t array_size,
    configRUN_TIME_COUNTER_TYPE *total_runtime
)
{
    if (total_runtime != NULL) {
        *total_runtime = 0U;
    }

    const UBaseType_t count = s_mock_task_count < array_size
        ? s_mock_task_count
        : array_size;
    for (UBaseType_t i = 0U; i < count; ++i) {
        task_status_array[i] = s_mock_task_statuses[i];
    }
    return count;
}


uint64_t ulTaskGetIdleRunTimeCounterForCore(BaseType_t core)
{
    (void)core;
    return 0U;
}


uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, uint32_t ticks_to_wait)
{
    (void)clear_on_exit;
    (void)ticks_to_wait;
    return 0U;
}


BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    return pdPASS;
}


BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task_code,
    const char *name,
    uint32_t stack_depth,
    void *parameters,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id,
    uint32_t memory_caps
)
{
    (void)task_code;
    (void)name;
    (void)stack_depth;
    (void)parameters;
    (void)priority;
    (void)core_id;
    (void)memory_caps;
    if (created_task != NULL) {
        *created_task = (TaskHandle_t)(uintptr_t)1U;
    }
    return pdPASS;
}


void vTaskDeleteWithCaps(TaskHandle_t task)
{
    (void)task;
}


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)(uintptr_t)1U;
}


BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks_to_wait)
{
    (void)semaphore;
    (void)ticks_to_wait;
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdTRUE;
}


void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}


static void test_percent_tenths_edges(void)
{
    assert(runtime_percent_tenths(1U, 0U) == 0U);
    assert(runtime_percent_tenths(1U, 2U) == 500U);
    assert(runtime_percent_tenths(1U, 3U) == 333U);
    assert(runtime_percent_tenths(2U, 1U) == 1000U);
    assert(runtime_percent_tenths(UINT64_MAX, UINT64_MAX) == 1000U);
    assert(runtime_percent_tenths(UINT64_MAX - 1U, UINT64_MAX) == 999U);

    const uint64_t even_denominator = UINT64_MAX - 1U;
    assert(
        runtime_percent_tenths(
            even_denominator / 2U,
            even_denominator
        ) == 500U
    );
    assert(
        runtime_percent_tenths(
            UINT64_MAX / 2U,
            UINT64_MAX
        ) == 499U
    );
}


static void test_percent_tenths_matches_direct_math_for_safe_small_ratios(void)
{
    for (uint64_t denominator = 1U; denominator <= 100U; ++denominator) {
        for (uint64_t numerator = 0U; numerator <= denominator + 1U; ++numerator) {
            const uint64_t scaled = numerator * 1000U;
            uint64_t expected = scaled / denominator;
            if (expected > 1000U) {
                expected = 1000U;
            }

            assert(
                runtime_percent_tenths(
                    numerator,
                    denominator
                ) == (unsigned)expected
            );
        }
    }
}


static void test_top_n_keeps_equal_runtime_entries_distinct_and_stable(void)
{
    runtime_task_usage_t top[RUNTIME_OBSERVABILITY_TOP_TASKS] = {0};
    TaskStatus_t statuses[12] = {0};
    size_t count = 0U;

    for (size_t i = 0U; i < 12U; ++i) {
        statuses[i].xHandle = (TaskHandle_t)(uintptr_t)(i + 1U);
        runtime_task_usage_insert(top, &count, &statuses[i], 100U, 10U);
    }

    assert(count == RUNTIME_OBSERVABILITY_TOP_TASKS);
    for (size_t i = 0U; i < count; ++i) {
        assert(top[i].status == &statuses[i]);
        assert(top[i].runtime_delta == 100U);
        for (size_t j = i + 1U; j < count; ++j) {
            assert(top[i].status != top[j].status);
        }
    }

    runtime_task_usage_insert(top, &count, &statuses[10], 150U, 20U);
    assert(count == RUNTIME_OBSERVABILITY_TOP_TASKS);
    assert(top[0].status == &statuses[10]);
    assert(top[0].runtime_delta == 150U);
    assert(top[1].status == &statuses[0]);
    assert(top[RUNTIME_OBSERVABILITY_TOP_TASKS - 1U].status == &statuses[8]);
}


static void test_status_arrays_prefer_psram(void)
{
    reset_runtime_storage();

    assert(runtime_status_arrays_ensure(4U));
    assert(s_status_capacity == 4U);
    assert(s_previous_status != NULL);
    assert(s_current_status != NULL);
    assert(s_spiram_allocs == 2U);
    assert(s_internal_allocs == 0U);
    assert(s_fallback_logs == 0U);
}


static void test_status_arrays_fall_back_to_internal_ram(void)
{
    reset_runtime_storage();
    s_fail_spiram = true;

    assert(runtime_status_arrays_ensure(5U));
    assert(s_status_capacity == 5U);
    assert(s_previous_status != NULL);
    assert(s_current_status != NULL);
    assert(s_spiram_allocs == 2U);
    assert(s_internal_allocs == 2U);
    assert(s_fallback_logs == 1U);
}


static void test_status_arrays_fail_cleanly_when_both_heaps_fail(void)
{
    reset_runtime_storage();
    s_fail_spiram = true;
    s_fail_internal = true;

    assert(!runtime_status_arrays_ensure(3U));
    assert(s_previous_status == NULL);
    assert(s_current_status == NULL);
    assert(s_status_capacity == 0U);
    assert(s_spiram_allocs == 2U);
    assert(s_internal_allocs == 2U);
}


static void test_memory_snapshot_queries_each_heap_semantic_once(void)
{
    reset_runtime_storage();

    runtime_observability_log_memory("host");

    assert(s_free_caps_internal_8bit == 1U);
    assert(s_free_caps_internal_dma == 1U);
    assert(s_free_caps_spiram == 1U);
    assert(s_free_caps_spiram_dma == 1U);
    assert(s_largest_caps_internal_8bit == 1U);
    assert(s_largest_caps_internal_dma == 1U);
    assert(s_largest_caps_spiram == 1U);
    assert(s_largest_caps_spiram_dma == 1U);
}


static void test_low_stack_warning_boundary_and_log_selection(void)
{
    assert(runtime_observability_stack_is_low(511U));
    assert(!runtime_observability_stack_is_low(512U));
    assert(!runtime_observability_stack_is_low(513U));
    assert(runtime_observability_stack_is_low(0U));

    reset_runtime_storage();
    s_snapshot_mutex = (SemaphoreHandle_t)(uintptr_t)1U;
    s_mock_task_count = 3U;
    s_mock_task_statuses[0].pcTaskName = "below";
    s_mock_task_statuses[0].usStackHighWaterMark = 511U;
    s_mock_task_statuses[1].pcTaskName = "at";
    s_mock_task_statuses[1].usStackHighWaterMark = 512U;
    s_mock_task_statuses[2].pcTaskName = "zero";
    s_mock_task_statuses[2].usStackHighWaterMark = 0U;

    runtime_observability_log_snapshot("host");

    assert(s_low_stack_warning_logs == 2U);
    assert(s_saw_low_stack_511);
    assert(s_saw_low_stack_0);
    assert(s_runtime_task_info_logs == 1U);
    assert(strstr(s_last_runtime_task_info, "RUNTIME_TASK rank=") != NULL);
    assert(strstr(s_last_runtime_task_info, "name=at") != NULL);
    assert(strstr(s_last_runtime_task_info, "stack_free=512") != NULL);
}


int main(void)
{
    test_percent_tenths_edges();
    test_percent_tenths_matches_direct_math_for_safe_small_ratios();
    test_top_n_keeps_equal_runtime_entries_distinct_and_stable();
    test_status_arrays_prefer_psram();
    test_status_arrays_fall_back_to_internal_ram();
    test_status_arrays_fail_cleanly_when_both_heaps_fail();
    test_memory_snapshot_queries_each_heap_semantic_once();
    test_low_stack_warning_boundary_and_log_selection();
    reset_runtime_storage();
    return 0;
}
