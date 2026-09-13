#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "measurement_service.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct
{
    size_t item_size;
    bool occupied;
    unsigned char item[160];

} fake_queue_t;


typedef struct
{
    UBaseType_t count;

} fake_semaphore_t;


static fake_queue_t *s_last_queue;
static bool s_fail_next_send;
static void (*s_delay_hook)(void);
static unsigned s_delay_calls;
static object_service_summary_t s_active_object;
static seismic_recorder_snapshot_t s_recorder;
static unsigned s_guard_calls;
static unsigned s_object_calls;
static unsigned s_release_a_calls;
static unsigned s_release_b_calls;
static unsigned s_refresh_a_calls;
static unsigned s_refresh_b_calls;
static bool s_guard_allowed;


QueueHandle_t xQueueCreate(size_t length, size_t item_size);
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout);
void vQueueDelete(QueueHandle_t handle);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t handle);
void vSemaphoreDelete(SemaphoreHandle_t handle);
BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id
);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);

esp_err_t object_service_get_active(object_service_summary_t *summary);
esp_err_t seismic_recorder_start(const char *object_id);
esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot);
esp_err_t seismic_recorder_stop(void);
esp_err_t seismic_recorder_wait_for_stop(void);
esp_err_t seismic_recorder_delete_session(const char *object_id, const char *session_id);

#include "../../components/measurement_service/measurement_service.c"


QueueHandle_t xQueueCreate(size_t length, size_t item_size)
{
    (void)length;
    fake_queue_t *queue = calloc(1U, sizeof(*queue));
    assert(queue != NULL);
    assert(item_size <= sizeof(queue->item));
    queue->item_size = item_size;
    s_last_queue = queue;
    return queue;
}


BaseType_t xQueueSend(
    QueueHandle_t handle,
    const void *item,
    TickType_t timeout
)
{
    (void)timeout;
    fake_queue_t *queue = handle;
    if (s_fail_next_send) {
        s_fail_next_send = false;
        return pdFALSE;
    }
    if (queue == NULL || queue->occupied) {
        return pdFALSE;
    }
    memcpy(queue->item, item, queue->item_size);
    queue->occupied = true;
    return pdTRUE;
}


BaseType_t xQueueReceive(
    QueueHandle_t handle,
    void *item,
    TickType_t timeout
)
{
    (void)timeout;
    fake_queue_t *queue = handle;
    if (queue == NULL || !queue->occupied) {
        return pdFALSE;
    }
    memcpy(item, queue->item, queue->item_size);
    queue->occupied = false;
    return pdTRUE;
}


void vQueueDelete(QueueHandle_t handle)
{
    free(handle);
    s_last_queue = NULL;
}


SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return calloc(1U, sizeof(fake_semaphore_t));
}


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    fake_semaphore_t *semaphore = calloc(1U, sizeof(*semaphore));
    if (semaphore != NULL) {
        semaphore->count = 1U;
    }
    return semaphore;
}


BaseType_t xSemaphoreTake(
    SemaphoreHandle_t handle,
    TickType_t timeout
)
{
    (void)timeout;
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL || semaphore->count == 0U) {
        return pdFALSE;
    }
    --semaphore->count;
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL) {
        return pdFALSE;
    }
    ++semaphore->count;
    return pdTRUE;
}


UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t handle)
{
    const fake_semaphore_t *semaphore = handle;
    return semaphore == NULL ? 0U : semaphore->count;
}


void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    free(handle);
}


BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id
)
{
    (void)task;
    (void)name;
    (void)stack_size;
    (void)argument;
    (void)priority;
    (void)core_id;
    *task_handle = (TaskHandle_t)1;
    return pdPASS;
}


void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}


void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    ++s_delay_calls;
    if (s_delay_hook != NULL) {
        void (*hook)(void) = s_delay_hook;
        s_delay_hook = NULL;
        hook();
    }
}


esp_err_t object_service_get_active(object_service_summary_t *summary)
{
    if (summary == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *summary = s_active_object;
    return ESP_OK;
}


esp_err_t seismic_recorder_start(const char *object_id)
{
    snprintf(s_recorder.object_id, sizeof(s_recorder.object_id), "%s", object_id);
    snprintf(s_recorder.session_id, sizeof(s_recorder.session_id), "S000001");
    s_recorder.running = true;
    s_recorder.state = SEISMIC_RECORDER_RECORDING;
    s_recorder.last_error = ESP_OK;
    s_recorder.stop_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_recorder.stop_stage_error = ESP_OK;
    return ESP_OK;
}


esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    *snapshot = s_recorder;
    return ESP_OK;
}


esp_err_t seismic_recorder_stop(void)
{
    s_recorder.state = SEISMIC_RECORDER_STOPPING;
    s_recorder.running = true;
    return ESP_OK;
}


esp_err_t seismic_recorder_wait_for_stop(void)
{
    s_recorder.state = SEISMIC_RECORDER_IDLE;
    s_recorder.running = false;
    s_recorder.stop_stage = SEISMIC_RECORDER_STOP_STAGE_COMPLETE;
    return ESP_OK;
}


esp_err_t seismic_recorder_delete_session(
    const char *object_id,
    const char *session_id
)
{
    (void)object_id;
    (void)session_id;
    return ESP_OK;
}


static void assert_callback_context(void)
{
    assert(fake_critical_depth == 0U);
    assert(s_mutex != NULL);
    assert(((fake_semaphore_t *)s_mutex)->count == 1U);
}


static esp_err_t guard_callback(void *user_context)
{
    assert(user_context == &s_guard_calls);
    assert_callback_context();
    ++s_guard_calls;
    return s_guard_allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}


static esp_err_t object_callback(
    const char *object_id,
    void *user_context
)
{
    assert(user_context == &s_object_calls);
    assert_callback_context();
    ++s_object_calls;
    snprintf(s_active_object.id, sizeof(s_active_object.id), "%s", object_id);
    s_active_object.active = true;
    return ESP_OK;
}


static void release_a_callback(void *user_context)
{
    assert(user_context == &s_release_a_calls);
    assert_callback_context();
    ++s_release_a_calls;
}


static void release_b_callback(void *user_context)
{
    assert(user_context == &s_release_b_calls);
    assert_callback_context();
    ++s_release_b_calls;
}


static void refresh_a_callback(void *user_context)
{
    assert(user_context == &s_refresh_a_calls);
    assert_callback_context();
    ++s_refresh_a_calls;
}


static void refresh_b_callback(void *user_context)
{
    assert(user_context == &s_refresh_b_calls);
    assert_callback_context();
    ++s_refresh_b_calls;
}


static void reset_fakes(void)
{
    s_fail_next_send = false;
    s_delay_hook = NULL;
    s_delay_calls = 0U;
    s_guard_calls = 0U;
    s_object_calls = 0U;
    s_release_a_calls = 0U;
    s_release_b_calls = 0U;
    s_refresh_a_calls = 0U;
    s_refresh_b_calls = 0U;
    s_guard_allowed = true;
    memset(&s_active_object, 0, sizeof(s_active_object));
    snprintf(s_active_object.id, sizeof(s_active_object.id), "OBJ_0001");
    snprintf(s_active_object.name, sizeof(s_active_object.name), "Test object");
    s_active_object.active = true;
    memset(&s_recorder, 0, sizeof(s_recorder));
    s_recorder.state = SEISMIC_RECORDER_IDLE;
}


static void start_service(void)
{
    reset_fakes();
    assert(measurement_service_init() == ESP_OK);
    /* The deterministic host harness drives queued commands explicitly. */
    s_task = NULL;
}


static void stop_service(void)
{
    assert(s_last_queue == NULL || !s_last_queue->occupied);
    measurement_service_shutdown();
}


static void run_pending_command(void)
{
    measurement_command_t command;
    assert(s_last_queue != NULL);
    assert(xQueueReceive(s_last_queue, &command, 0) == pdTRUE);
    assert(command.type == MEASUREMENT_COMMAND_START);
    measurement_service_process_start(&command);
    measurement_service_release_gate();
}


static void delayed_run_pending_command(void)
{
    run_pending_command();
}


static measurement_hook_type_t s_delayed_release_hook;


static void delayed_release_hook(void)
{
    measurement_service_hook_release(s_delayed_release_hook);
}


static void test_callbacks_run_outside_locks(void)
{
    start_service();

    assert(measurement_service_set_start_guard(
        guard_callback,
        &s_guard_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_object_callback(
        object_callback,
        &s_object_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        release_a_callback,
        &s_release_a_calls
    ) == ESP_OK);
    assert(measurement_service_set_storage_refresh_callback(
        refresh_a_callback,
        &s_refresh_a_calls
    ) == ESP_OK);

    assert(measurement_service_start_object_async("OBJ_0002") == ESP_OK);
    assert(s_guard_calls == 1U);
    run_pending_command();
    assert(s_object_calls == 1U);
    assert(s_release_a_calls == 1U);

    measurement_service_notify_storage_refresh();
    assert(s_refresh_a_calls == 1U);
    assert(fake_critical_depth == 0U);

    stop_service();
}


static void test_object_unregister_waits_for_queued_start(void)
{
    start_service();
    assert(measurement_service_set_start_object_callback(
        object_callback,
        &s_object_calls
    ) == ESP_OK);

    assert(measurement_service_start_object_async("OBJ_0002") == ESP_OK);
    assert(s_hook_in_flight[MEASUREMENT_HOOK_START_OBJECT] == 1U);

    s_delay_hook = delayed_run_pending_command;
    assert(measurement_service_set_start_object_callback(NULL, NULL) == ESP_OK);

    assert(s_delay_calls >= 1U);
    assert(s_object_calls == 1U);
    assert(s_hook_in_flight[MEASUREMENT_HOOK_START_OBJECT] == 0U);
    assert(s_start_object_callback == NULL);

    stop_service();
}


static void test_reservation_keeps_exact_release_hook(void)
{
    start_service();
    assert(measurement_service_set_start_guard(
        guard_callback,
        &s_guard_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        release_a_callback,
        &s_release_a_calls
    ) == ESP_OK);

    assert(measurement_service_start_async() == ESP_OK);
    assert(s_start_reservation_held);
    assert(
        s_hook_in_flight[MEASUREMENT_HOOK_START_RESERVATION_RELEASE] == 1U
    );

    s_delay_hook = delayed_run_pending_command;
    assert(measurement_service_set_start_reservation_release_callback(
        release_b_callback,
        &s_release_b_calls
    ) == ESP_OK);

    assert(s_release_a_calls == 1U);
    assert(s_release_b_calls == 0U);
    assert(!s_start_reservation_held);
    assert(
        s_hook_in_flight[MEASUREMENT_HOOK_START_RESERVATION_RELEASE] == 0U
    );
    assert(s_start_reservation_release_callback == release_b_callback);
    assert(s_start_reservation_release_context == &s_release_b_calls);

    stop_service();
}


static void test_guard_rejection_releases_snapshots_without_release_callback(void)
{
    start_service();
    s_guard_allowed = false;

    assert(measurement_service_set_start_guard(
        guard_callback,
        &s_guard_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_object_callback(
        object_callback,
        &s_object_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        release_a_callback,
        &s_release_a_calls
    ) == ESP_OK);

    assert(
        measurement_service_start_object_async("OBJ_0002") ==
        ESP_ERR_INVALID_STATE
    );
    assert(s_guard_calls == 1U);
    assert(s_object_calls == 0U);
    assert(s_release_a_calls == 0U);
    assert(!s_start_reservation_held);
    for (size_t i = 0U; i < MEASUREMENT_HOOK_COUNT; ++i) {
        assert(s_hook_in_flight[i] == 0U);
    }
    assert(!measurement_service_is_busy());

    stop_service();
}


static void test_queue_failure_releases_object_and_reservation_ownership(void)
{
    start_service();

    assert(measurement_service_set_start_guard(
        guard_callback,
        &s_guard_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_object_callback(
        object_callback,
        &s_object_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        release_a_callback,
        &s_release_a_calls
    ) == ESP_OK);

    s_fail_next_send = true;
    assert(
        measurement_service_start_object_async("OBJ_0002") ==
        ESP_ERR_TIMEOUT
    );
    assert(s_guard_calls == 1U);
    assert(s_object_calls == 0U);
    assert(s_release_a_calls == 1U);
    assert(!s_start_reservation_held);
    assert(s_hook_in_flight[MEASUREMENT_HOOK_START_OBJECT] == 0U);
    assert(
        s_hook_in_flight[MEASUREMENT_HOOK_START_RESERVATION_RELEASE] == 0U
    );
    assert(!measurement_service_is_busy());

    stop_service();
}


static void test_guard_and_refresh_setters_are_lifetime_barriers(void)
{
    start_service();

    assert(measurement_service_set_start_guard(
        guard_callback,
        &s_guard_calls
    ) == ESP_OK);
    s_hook_in_flight[MEASUREMENT_HOOK_START_GUARD] = 1U;
    s_delayed_release_hook = MEASUREMENT_HOOK_START_GUARD;
    s_delay_hook = delayed_release_hook;
    assert(measurement_service_set_start_guard(NULL, NULL) == ESP_OK);
    assert(s_hook_in_flight[MEASUREMENT_HOOK_START_GUARD] == 0U);
    assert(s_start_guard == NULL);

    assert(measurement_service_set_storage_refresh_callback(
        refresh_a_callback,
        &s_refresh_a_calls
    ) == ESP_OK);
    measurement_service_notify_storage_refresh();
    assert(s_refresh_a_calls == 1U);

    s_hook_in_flight[MEASUREMENT_HOOK_STORAGE_REFRESH] = 1U;
    s_delayed_release_hook = MEASUREMENT_HOOK_STORAGE_REFRESH;
    s_delay_hook = delayed_release_hook;
    assert(measurement_service_set_storage_refresh_callback(
        refresh_b_callback,
        &s_refresh_b_calls
    ) == ESP_OK);
    assert(s_hook_in_flight[MEASUREMENT_HOOK_STORAGE_REFRESH] == 0U);
    assert(s_storage_refresh_callback == refresh_b_callback);
    assert(s_storage_refresh_context == &s_refresh_b_calls);

    measurement_service_notify_storage_refresh();
    assert(s_refresh_b_calls == 1U);

    stop_service();
}


static void test_shutdown_closes_all_hook_slots(void)
{
    start_service();

    assert(measurement_service_set_start_guard(
        guard_callback,
        &s_guard_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_object_callback(
        object_callback,
        &s_object_calls
    ) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        release_a_callback,
        &s_release_a_calls
    ) == ESP_OK);
    assert(measurement_service_set_storage_refresh_callback(
        refresh_a_callback,
        &s_refresh_a_calls
    ) == ESP_OK);

    measurement_service_shutdown();

    assert(!s_initialized);
    assert(s_start_guard == NULL);
    assert(s_start_object_callback == NULL);
    assert(s_start_reservation_release_callback == NULL);
    assert(s_storage_refresh_callback == NULL);
    for (size_t i = 0U; i < MEASUREMENT_HOOK_COUNT; ++i) {
        assert(s_hook_in_flight[i] == 0U);
    }
    assert(fake_critical_depth == 0U);
}


int main(void)
{
    test_callbacks_run_outside_locks();
    test_object_unregister_waits_for_queued_start();
    test_reservation_keeps_exact_release_hook();
    test_guard_rejection_releases_snapshots_without_release_callback();
    test_queue_failure_releases_object_and_reservation_ownership();
    test_guard_and_refresh_setters_are_lifetime_barriers();
    test_shutdown_closes_all_hook_slots();
    return 0;
}
