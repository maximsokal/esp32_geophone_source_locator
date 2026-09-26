#include "runtime_observability.h"

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"

#ifdef ESP_PLATFORM

#include <string.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#endif


static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static runtime_observability_task_status_t s_tasks[RUNTIME_TASK_COUNT];


void runtime_observability_beat(
    runtime_observability_task_t task,
    uint64_t progress
)
{
    if (task < 0 || task >= RUNTIME_TASK_COUNT) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);

    runtime_observability_task_status_t *status = &s_tasks[task];
    status->observed = true;
    status->heartbeat++;
    status->progress = progress;
    status->last_heartbeat_ms = now_us > 0
        ? (uint64_t)(now_us / 1000LL)
        : 0U;

    portEXIT_CRITICAL(&s_lock);
}


void runtime_observability_get_snapshot(
    runtime_observability_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    /* Read the clock while the task table is locked. A beat that occurs
     * concurrently must never receive a timestamp newer than this snapshot. */
    const int64_t now_us = esp_timer_get_time();
    snapshot->now_ms = now_us > 0
        ? (uint64_t)(now_us / 1000LL)
        : 0U;
    for (size_t i = 0; i < RUNTIME_TASK_COUNT; ++i) {
        snapshot->tasks[i] = s_tasks[i];
    }
    portEXIT_CRITICAL(&s_lock);
}


void runtime_observability_get_heap_snapshot(
    runtime_observability_heap_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return;
    }

#ifdef ESP_PLATFORM
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    *snapshot = (runtime_observability_heap_snapshot_t) {
        .free_internal = heap_caps_get_free_size(internal_caps),
        .largest_internal_block = heap_caps_get_largest_free_block(internal_caps),
        .free_dma = heap_caps_get_free_size(dma_caps),
        .largest_dma_block = heap_caps_get_largest_free_block(dma_caps),
        .free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .min_free_internal_since_boot = heap_caps_get_minimum_free_size(internal_caps),
    };
#else
    *snapshot = (runtime_observability_heap_snapshot_t) { 0 };
#endif
}


const char *runtime_observability_task_name(
    runtime_observability_task_t task
)
{
    switch (task) {
        case RUNTIME_TASK_ACQUISITION:
            return "acquisition";
        case RUNTIME_TASK_RECORDER:
            return "recorder";
        case RUNTIME_TASK_UI:
            return "ui";
        case RUNTIME_TASK_COUNT:
        default:
            return "unknown";
    }
}


#ifdef ESP_PLATFORM


static const char *TAG = "RUNTIME_OBSERVABILITY";

#define RUNTIME_OBSERVABILITY_PERIOD_MS 10000U
#define RUNTIME_OBSERVABILITY_CPU_COUNT 2U
#define RUNTIME_OBSERVABILITY_STAGE_SIZE 32U
#define RUNTIME_OBSERVABILITY_TOP_TASKS 10U

static SemaphoreHandle_t s_snapshot_mutex = NULL;
static TaskHandle_t s_snapshot_task = NULL;
static TaskStatus_t *s_previous_status = NULL;
static TaskStatus_t *s_current_status = NULL;
static UBaseType_t s_status_capacity = 0U;
static UBaseType_t s_previous_count = 0U;
static uint64_t s_previous_snapshot_us = 0U;
static uint64_t s_previous_idle_us[RUNTIME_OBSERVABILITY_CPU_COUNT];
static bool s_have_previous_snapshot = false;
static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_requested_stage[RUNTIME_OBSERVABILITY_STAGE_SIZE];
static bool s_snapshot_requested = false;


static const char *runtime_stage_or_unknown(const char *stage)
{
    return stage != NULL ? stage : "unknown";
}


static void runtime_copy_stage(
    char *destination,
    size_t destination_size,
    const char *stage
)
{
    const char *source = runtime_stage_or_unknown(stage);
    const size_t length = strnlen(source, destination_size - 1U);
    memcpy(destination, source, length);
    destination[length] = '\0';
}


static bool runtime_take_requested_stage(
    char *stage,
    size_t stage_size
)
{
    bool requested = false;

    portENTER_CRITICAL(&s_request_lock);
    if (s_snapshot_requested) {
        runtime_copy_stage(stage, stage_size, s_requested_stage);
        s_snapshot_requested = false;
        requested = true;
    }
    portEXIT_CRITICAL(&s_request_lock);

    return requested;
}


static uint64_t runtime_percent_threshold(
    uint64_t denominator,
    unsigned tenths
)
{
    /*
     * runtime_percent_tenths() only asks for thresholds 1..999. Splitting the
     * denominator first keeps both products below UINT64_MAX while computing
     * ceil(tenths * denominator / 1000) exactly.
     */
    const uint64_t whole =
        denominator / 1000U;
    const uint64_t remainder =
        denominator % 1000U;
    const uint64_t remainder_scaled =
        (uint64_t)tenths * remainder;
    const uint64_t remainder_ceil =
        (remainder_scaled + 999U) / 1000U;

    return
        (uint64_t)tenths * whole +
        remainder_ceil;
}


static unsigned runtime_percent_tenths(
    uint64_t numerator,
    uint64_t denominator
)
{
    if (denominator == 0U) {
        return 0U;
    }

    if (numerator >= denominator) {
        return 1000U;
    }

    /*
     * We need floor(numerator * 1000 / denominator), but numerator * 1000 can
     * overflow uint64_t for long-running counters. Binary-search the bounded
     * result instead. For a candidate k, the exact relation is:
     *
     *   floor(n * 1000 / d) >= k
     *     iff n >= ceil(k * d / 1000)
     *
     * runtime_percent_threshold() evaluates the right-hand side without any
     * overflowing multiplication. Ten iterations cover the full 0..999 range.
     */
    unsigned low = 0U;
    unsigned high = 999U;

    while (low < high) {
        const unsigned middle =
            low + (high - low + 1U) / 2U;

        if (
            numerator >=
            runtime_percent_threshold(
                denominator,
                middle
            )
        ) {
            low = middle;
        }
        else {
            high = middle - 1U;
        }
    }

    return low;
}


typedef struct
{
    const TaskStatus_t *status;
    uint64_t runtime_delta;
    unsigned cpu_tenths;

} runtime_task_usage_t;


static void runtime_task_usage_insert(
    runtime_task_usage_t top_tasks[RUNTIME_OBSERVABILITY_TOP_TASKS],
    size_t *top_count,
    const TaskStatus_t *status,
    uint64_t runtime_delta,
    unsigned cpu_tenths
)
{
    if (top_tasks == NULL || top_count == NULL || status == NULL) {
        return;
    }

    if (
        *top_count >= RUNTIME_OBSERVABILITY_TOP_TASKS &&
        runtime_delta <= top_tasks[RUNTIME_OBSERVABILITY_TOP_TASKS - 1U].runtime_delta
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

    top_tasks[position].status = status;
    top_tasks[position].runtime_delta = runtime_delta;
    top_tasks[position].cpu_tenths = cpu_tenths;

    if (*top_count < RUNTIME_OBSERVABILITY_TOP_TASKS) {
        ++(*top_count);
    }
}


static bool runtime_status_arrays_ensure(UBaseType_t task_count)
{
    if (task_count == 0U) {
        task_count = 1U;
    }

    if (
        s_current_status != NULL &&
        s_previous_status != NULL &&
        s_status_capacity >= task_count
    ) {
        return true;
    }

    const size_t bytes =
        (size_t)task_count * sizeof(TaskStatus_t);

    TaskStatus_t *new_previous =
        heap_caps_malloc(
            bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    TaskStatus_t *new_current =
        heap_caps_malloc(
            bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (new_previous == NULL || new_current == NULL) {
        heap_caps_free(new_previous);
        heap_caps_free(new_current);

        /* Diagnostics must remain useful on a board without enough PSRAM,
         * but this fallback is explicit and visible in the UART log. */
        new_previous =
            heap_caps_malloc(
                bytes,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            );
        new_current =
            heap_caps_malloc(
                bytes,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            );

        if (new_previous == NULL || new_current == NULL) {
            heap_caps_free(new_previous);
            heap_caps_free(new_current);
            return false;
        }

        ESP_LOGW(
            TAG,
            "RUNTIME_SNAPSHOT_STATUS_STORAGE fallback=internal bytes=%u",
            (unsigned)(bytes * 2U)
        );
    }

    if (s_previous_status != NULL && s_previous_count > 0U) {
        memcpy(
            new_previous,
            s_previous_status,
            (size_t)s_previous_count * sizeof(TaskStatus_t)
        );
    }

    heap_caps_free(s_previous_status);
    heap_caps_free(s_current_status);
    s_previous_status = new_previous;
    s_current_status = new_current;
    s_status_capacity = task_count;
    return true;
}


static const TaskStatus_t *runtime_find_previous(
    const TaskStatus_t *current
)
{
    if (current == NULL) {
        return NULL;
    }

    for (UBaseType_t i = 0U; i < s_previous_count; ++i) {
        if (s_previous_status[i].xHandle == current->xHandle) {
            return &s_previous_status[i];
        }
    }

    return NULL;
}


static int runtime_task_core(const TaskStatus_t *status)
{
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    if (status->xCoreID == tskNO_AFFINITY) {
        return -1;
    }
    return (int)status->xCoreID;
#else
    (void)status;
    return -1;
#endif
}


void runtime_observability_log_memory(const char *stage)
{
    const uint32_t internal_caps =
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps =
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const uint32_t psram_caps =
        MALLOC_CAP_SPIRAM;
    const uint32_t psram_dma_caps =
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;

    ESP_LOGI(
        TAG,
        "MEM_RUNTIME stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u psram_dma_free=%u psram_dma_largest=%u",
        runtime_stage_or_unknown(stage),
        (unsigned)heap_caps_get_free_size(internal_caps),
        (unsigned)heap_caps_get_largest_free_block(internal_caps),
        (unsigned)heap_caps_get_free_size(dma_caps),
        (unsigned)heap_caps_get_largest_free_block(dma_caps),
        (unsigned)heap_caps_get_free_size(psram_caps),
        (unsigned)heap_caps_get_largest_free_block(psram_caps),
        (unsigned)heap_caps_get_free_size(psram_dma_caps),
        (unsigned)heap_caps_get_largest_free_block(psram_dma_caps)
    );
}


void runtime_observability_log_snapshot(const char *stage)
{
    runtime_observability_log_memory(stage);

    if (s_snapshot_mutex == NULL) {
        ESP_LOGW(
            TAG,
            "RUNTIME_SNAPSHOT_ERROR stage=%s reason=not_started",
            runtime_stage_or_unknown(stage)
        );
        return;
    }

    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(
            TAG,
            "RUNTIME_SNAPSHOT_ERROR stage=%s reason=mutex",
            runtime_stage_or_unknown(stage)
        );
        return;
    }

    const UBaseType_t requested_count = uxTaskGetNumberOfTasks();
    if (!runtime_status_arrays_ensure(requested_count)) {
        xSemaphoreGive(s_snapshot_mutex);
        ESP_LOGW(
            TAG,
            "RUNTIME_SNAPSHOT_ERROR stage=%s reason=status_alloc tasks=%u",
            runtime_stage_or_unknown(stage),
            (unsigned)requested_count
        );
        return;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0U;
    const UBaseType_t count =
        uxTaskGetSystemState(
            s_current_status,
            s_status_capacity,
            &total_runtime
        );

    const uint64_t now_us =
        (uint64_t)esp_timer_get_time();
    const uint64_t window_us =
        s_have_previous_snapshot && now_us > s_previous_snapshot_us
            ? now_us - s_previous_snapshot_us
            : 0U;

    uint64_t idle_us[RUNTIME_OBSERVABILITY_CPU_COUNT] = {0U, 0U};
    unsigned idle_tenths[RUNTIME_OBSERVABILITY_CPU_COUNT] = {0U, 0U};
    unsigned busy_tenths[RUNTIME_OBSERVABILITY_CPU_COUNT] = {0U, 0U};

    for (unsigned core = 0U; core < RUNTIME_OBSERVABILITY_CPU_COUNT; ++core) {
        idle_us[core] =
            (uint64_t)ulTaskGetIdleRunTimeCounterForCore((BaseType_t)core);

        if (s_have_previous_snapshot && idle_us[core] >= s_previous_idle_us[core]) {
            idle_tenths[core] = runtime_percent_tenths(
                idle_us[core] - s_previous_idle_us[core],
                window_us
            );
        }

        busy_tenths[core] = 1000U - idle_tenths[core];
    }

    ESP_LOGI(
        TAG,
        "RUNTIME_CPU stage=%s window_ms=%u valid=%d cpu0_busy=%u.%u cpu0_idle=%u.%u cpu1_busy=%u.%u cpu1_idle=%u.%u",
        runtime_stage_or_unknown(stage),
        (unsigned)(window_us / 1000U),
        s_have_previous_snapshot ? 1 : 0,
        busy_tenths[0] / 10U,
        busy_tenths[0] % 10U,
        idle_tenths[0] / 10U,
        idle_tenths[0] % 10U,
        busy_tenths[1] / 10U,
        busy_tenths[1] % 10U,
        idle_tenths[1] / 10U,
        idle_tenths[1] % 10U
    );

    runtime_task_usage_t top_tasks[RUNTIME_OBSERVABILITY_TOP_TASKS] = {0};
    size_t top_count = 0U;

    for (UBaseType_t i = 0U; i < count; ++i) {
        const TaskStatus_t *status = &s_current_status[i];

        const TaskStatus_t *previous =
            s_have_previous_snapshot
                ? runtime_find_previous(status)
                : NULL;
        const uint64_t previous_runtime =
            previous != NULL
                ? (uint64_t)previous->ulRunTimeCounter
                : (uint64_t)status->ulRunTimeCounter;
        const uint64_t current_runtime =
            (uint64_t)status->ulRunTimeCounter;
        const uint64_t runtime_delta =
            current_runtime >= previous_runtime
                ? current_runtime - previous_runtime
                : 0U;
        const unsigned cpu_tenths =
            previous != NULL
                ? runtime_percent_tenths(runtime_delta, window_us)
                : 0U;

        runtime_task_usage_insert(
            top_tasks,
            &top_count,
            status,
            runtime_delta,
            cpu_tenths
        );
    }

    for (size_t i = 0U; i < top_count; ++i) {
        const runtime_task_usage_t *usage = &top_tasks[i];
        const TaskStatus_t *status = usage->status;
        ESP_LOGI(
            TAG,
            "RUNTIME_TASK rank=%u name=%s core=%d priority_current=%u priority_base=%u inherited=%u runtime_delta_us=%llu cpu=%u.%u stack_free=%u",
            (unsigned)(i + 1U),
            status->pcTaskName != NULL ? status->pcTaskName : "?",
            runtime_task_core(status),
            (unsigned)status->uxCurrentPriority,
            (unsigned)status->uxBasePriority,
            status->uxCurrentPriority > status->uxBasePriority ? 1U : 0U,
            (unsigned long long)usage->runtime_delta,
            usage->cpu_tenths / 10U,
            usage->cpu_tenths % 10U,
            (unsigned)status->usStackHighWaterMark
        );
    }

    if (count > 0U) {
        memcpy(
            s_previous_status,
            s_current_status,
            (size_t)count * sizeof(TaskStatus_t)
        );
    }

    TaskStatus_t *swapped = s_previous_status;
    s_previous_status = s_current_status;
    s_current_status = swapped;
    s_previous_count = count;
    s_previous_snapshot_us = now_us;
    for (unsigned core = 0U; core < RUNTIME_OBSERVABILITY_CPU_COUNT; ++core) {
        s_previous_idle_us[core] = idle_us[core];
    }
    s_have_previous_snapshot = true;

    xSemaphoreGive(s_snapshot_mutex);
}


static void runtime_observability_task(void *argument)
{
    (void)argument;

    for (;;) {
        const uint32_t notified = ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(RUNTIME_OBSERVABILITY_PERIOD_MS)
        );
        char requested_stage[RUNTIME_OBSERVABILITY_STAGE_SIZE];
        if (runtime_take_requested_stage(requested_stage, sizeof(requested_stage))) {
            runtime_observability_log_snapshot(requested_stage);
        } else if (notified == 0U) {
            runtime_observability_log_snapshot("periodic");
        }
    }
}


void runtime_observability_request_snapshot(const char *stage)
{
    if (s_snapshot_task == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_request_lock);
    runtime_copy_stage(s_requested_stage, sizeof(s_requested_stage), stage);
    s_snapshot_requested = true;
    portEXIT_CRITICAL(&s_request_lock);

    (void)xTaskNotifyGive(s_snapshot_task);
}


bool runtime_observability_start(void)
{
    if (s_snapshot_task != NULL) {
        return true;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) {
        ESP_LOGE(TAG, "RUNTIME_START_ERROR reason=mutex_alloc");
        return false;
    }

    const BaseType_t result =
        xTaskCreatePinnedToCoreWithCaps(
            runtime_observability_task,
            "runtime_observability",
            APP_TASK_STACK_RUNTIME_OBSERVABILITY,
            NULL,
            APP_TASK_PRIORITY_RUNTIME_OBSERVABILITY,
            &s_snapshot_task,
            APP_TASK_CORE_SERVICES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (result != pdPASS) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        ESP_LOGE(TAG, "RUNTIME_START_ERROR reason=task_alloc");
        return false;
    }

    ESP_LOGI(
        TAG,
        "RUNTIME_START core=%d period_ms=%u stack=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES,
        RUNTIME_OBSERVABILITY_PERIOD_MS,
        APP_TASK_STACK_RUNTIME_OBSERVABILITY
    );
    return true;
}


void runtime_observability_stop(void)
{
    portENTER_CRITICAL(&s_request_lock);
    s_snapshot_requested = false;
    s_requested_stage[0] = '\0';
    portEXIT_CRITICAL(&s_request_lock);

    if (s_snapshot_mutex != NULL) {
        (void)xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    }

    if (s_snapshot_task != NULL) {
        vTaskDeleteWithCaps(s_snapshot_task);
        s_snapshot_task = NULL;
    }

    heap_caps_free(s_previous_status);
    heap_caps_free(s_current_status);
    s_previous_status = NULL;
    s_current_status = NULL;
    s_status_capacity = 0U;
    s_previous_count = 0U;
    s_previous_snapshot_us = 0U;
    s_have_previous_snapshot = false;

    if (s_snapshot_mutex != NULL) {
        xSemaphoreGive(s_snapshot_mutex);
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
    }
}


#else


bool runtime_observability_start(void)
{
    return true;
}


void runtime_observability_stop(void)
{
}


void runtime_observability_log_snapshot(const char *stage)
{
    (void)stage;
}


void runtime_observability_log_memory(const char *stage)
{
    (void)stage;
}


void runtime_observability_request_snapshot(const char *stage)
{
    (void)stage;
}


#endif
