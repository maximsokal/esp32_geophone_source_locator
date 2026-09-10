/* Exercise the production worker handlers with boundary fakes. */
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

typedef struct {
    size_t length;
    size_t item_size;
    bool occupied;
    unsigned char item[8];
} fake_queue_t;

typedef struct {
    UBaseType_t count;
} fake_semaphore_t;

static fake_queue_t *last_queue;
static esp_err_t fake_object_error;
static bool fake_active_object;
static esp_err_t fake_recorder_start_error;
static esp_err_t fake_recorder_snapshot_error;
static esp_err_t fake_recorder_stop_error;
static esp_err_t fake_recorder_delete_error;
static bool fake_recorder_stop_pending;
static unsigned fake_recorder_start_calls;
static unsigned fake_recorder_stop_calls;
static unsigned fake_recorder_delete_calls;
static object_service_summary_t fake_object;
static seismic_recorder_snapshot_t fake_recorder;

QueueHandle_t xQueueCreate(size_t length, size_t item_size)
{
    fake_queue_t *queue = calloc(1, sizeof(*queue));
    if (queue != NULL) {
        queue->length = length;
        queue->item_size = item_size;
        last_queue = queue;
    }
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout)
{
    (void)timeout;
    fake_queue_t *queue = handle;
    if (queue == NULL || queue->occupied) {
        return pdFALSE;
    }
    memcpy(queue->item, item, queue->item_size);
    queue->occupied = true;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout)
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
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return calloc(1, sizeof(fake_semaphore_t));
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    fake_semaphore_t *semaphore = calloc(1, sizeof(*semaphore));
    if (semaphore != NULL) {
        semaphore->count = 1U;
    }
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    (void)timeout;
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL || semaphore->count == 0U) {
        return pdFALSE;
    }
    semaphore->count--;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL) {
        return pdFALSE;
    }
    semaphore->count++;
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

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id,
    unsigned memory_caps
)
{
    (void)memory_caps;
    return xTaskCreatePinnedToCore(
        task,
        name,
        stack_size,
        argument,
        priority,
        task_handle,
        core_id
    );
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    vTaskDelete(task);
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t object_service_get_active(object_service_summary_t *summary)
{
    if (summary == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fake_object_error != ESP_OK) {
        return fake_object_error;
    }
    *summary = fake_object;
    summary->active = fake_active_object;
    return ESP_OK;
}

esp_err_t seismic_recorder_start(const char *object_id)
{
    ++fake_recorder_start_calls;
    if (fake_recorder_start_error != ESP_OK) {
        return fake_recorder_start_error;
    }
    snprintf(fake_recorder.object_id, sizeof(fake_recorder.object_id), "%s", object_id);
    snprintf(fake_recorder.session_id, sizeof(fake_recorder.session_id), "S000001");
    fake_recorder.running = true;
    fake_recorder.state = SEISMIC_RECORDER_RECORDING;
    fake_recorder.stop_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    fake_recorder.stop_stage_error = ESP_OK;
    fake_recorder.sample_rate_hz = 50U;
    fake_recorder.channel_count = 4U;
    fake_recorder.samples_written = 13U;
    fake_recorder.dropped_frames = 2U;
    fake_recorder.recording_ms = 260U;
    return ESP_OK;
}

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    if (fake_recorder_snapshot_error != ESP_OK) {
        return fake_recorder_snapshot_error;
    }
    *snapshot = fake_recorder;
    return ESP_OK;
}

esp_err_t seismic_recorder_stop(void)
{
    ++fake_recorder_stop_calls;
    if (fake_recorder_stop_error != ESP_OK) {
        return fake_recorder_stop_error;
    }
    fake_recorder_stop_pending = true;
    fake_recorder.state = SEISMIC_RECORDER_STOPPING;
    fake_recorder.stop_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    fake_recorder.stop_stage_error = ESP_OK;
    return ESP_OK;
}

esp_err_t seismic_recorder_wait_for_stop(void)
{
    if (fake_recorder_stop_pending) {
        fake_recorder_stop_pending = false;
        fake_recorder.running = false;
        fake_recorder.state =
            fake_recorder.last_error == ESP_OK
                ? SEISMIC_RECORDER_IDLE
                : SEISMIC_RECORDER_ERROR;
        fake_recorder.stop_stage =
            fake_recorder.last_error == ESP_OK
                ? SEISMIC_RECORDER_STOP_STAGE_COMPLETE
                : SEISMIC_RECORDER_STOP_STAGE_ERROR;
        if (fake_recorder.last_error != ESP_OK) {
            fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
            fake_recorder.stop_stage_error = fake_recorder.last_error;
        }
    }
    return fake_recorder.last_error;
}

esp_err_t seismic_recorder_delete_session(const char *object_id, const char *session_id)
{
    (void)object_id;
    (void)session_id;
    ++fake_recorder_delete_calls;
    return fake_recorder_delete_error;
}

#include "../measurement_service.c"

static void reset_fakes(void)
{
    fake_object_error = ESP_OK;
    fake_active_object = true;
    memset(&fake_object, 0, sizeof(fake_object));
    snprintf(fake_object.id, sizeof(fake_object.id), "OBJ_0001");
    snprintf(fake_object.name, sizeof(fake_object.name), "Test object");
    memset(&fake_recorder, 0, sizeof(fake_recorder));
    fake_recorder_snapshot_error = ESP_OK;
    fake_recorder_start_error = ESP_OK;
    fake_recorder_stop_error = ESP_OK;
    fake_recorder_delete_error = ESP_OK;
    fake_recorder_stop_pending = false;
    fake_recorder_start_calls = 0U;
    fake_recorder_stop_calls = 0U;
    fake_recorder_delete_calls = 0U;
}

static void start_service(void)
{
    reset_fakes();
    assert(measurement_service_init() == ESP_OK);
    /* The fake scheduler does not run the worker task. */
    s_task = NULL;
}

static void stop_service(void)
{
    measurement_service_shutdown();
    last_queue = NULL;
}

static void run_pending_command(void)
{
    measurement_command_t command;
    assert(last_queue != NULL);
    assert(xQueueReceive(last_queue, &command, 0) == pdTRUE);
    switch (command) {
        case MEASUREMENT_COMMAND_START: measurement_service_process_start(); break;
        case MEASUREMENT_COMMAND_STOP: measurement_service_process_stop(); break;
        case MEASUREMENT_COMMAND_KEEP: measurement_service_process_keep(); break;
        case MEASUREMENT_COMMAND_DELETE: measurement_service_process_delete(); break;
        default: assert(false); break;
    }
    measurement_service_release_gate();
}

static measurement_service_snapshot_t snapshot(void)
{
    measurement_service_snapshot_t result;
    assert(measurement_service_get_snapshot(&result) == ESP_OK);
    return result;
}

static void test_initial_and_no_active_object(void)
{
    start_service();
    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(current.active_object_present);

    fake_active_object = false;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.last_error == ESP_ERR_NOT_FOUND);
    assert(fake_recorder_start_calls == 0U);
    stop_service();
}

static void test_start_stop_keep_delete_and_repeated_start(void)
{
    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    assert(measurement_service_is_busy());
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);
    run_pending_command();
    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);
    assert(strcmp(current.session_id, "S000001") == 0);
    assert(current.samples_written == 13U);
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);
    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_STOPPING);
    assert(current.stop_stage == SEISMIC_RECORDER_STOP_STAGE_DRAIN);
    assert(fake_recorder_stop_calls == 1U);
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);
    assert(measurement_service_keep_async() == ESP_ERR_INVALID_STATE);
    assert(measurement_service_delete_async() == ESP_ERR_INVALID_STATE);

    /* 50 x 100 ms service polls model completion beyond five seconds. */
    for (unsigned poll = 0U; poll < 50U; ++poll) {
        measurement_service_process_stop_poll();
    }
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_STOPPING);
    assert(current.last_error == ESP_OK);

    assert(fake_recorder_stop_pending);
    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_REVIEW);
    assert(current.stop_stage == SEISMIC_RECORDER_STOP_STAGE_COMPLETE);

    assert(measurement_service_keep_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(current.session_id[0] == '\0');

    /* DELETE is accepted only from REVIEW and returns to LIVE. */
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_STOPPING);
    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    assert(measurement_service_delete_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(fake_recorder_delete_calls == 1U);
    stop_service();
}

static void test_retry_start_after_error_keeps_gate_until_worker_runs(void)
{
    start_service();
    fake_recorder_start_error = ESP_FAIL;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();

    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(!current.busy);

    /* The accepted retry still reports the previous ERROR until the worker
     * dequeues it; the submit gate marks that stale state as in flight. */
    fake_recorder_start_error = ESP_OK;
    assert(measurement_service_start_async() == ESP_OK);
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.busy);

    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);
    assert(!current.busy);
    stop_service();
}

static void test_underlying_errors_and_queue_full(void)
{
    start_service();
    fake_recorder_start_error = ESP_FAIL;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.last_error == ESP_FAIL);

    /* Restore the live state and fill the one-slot queue independently. */
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_LIVE;
    measurement_service_unlock();
    assert(measurement_service_start_async() == ESP_OK);
    fake_semaphore_t *gate = s_gate;
    assert(xSemaphoreGive(gate) == pdTRUE);
    assert(measurement_service_start_async() == ESP_ERR_TIMEOUT);
    assert(last_queue->occupied);
    stop_service();

    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    fake_recorder_stop_error = ESP_FAIL;
    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.last_error == ESP_FAIL);
    stop_service();
}

static void test_snapshot_and_delete_errors_preserve_safe_states(void)
{
    start_service();
    fake_recorder_snapshot_error = ESP_FAIL;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.last_error == ESP_FAIL);
    assert(fake_recorder_stop_calls == 1U);
    stop_service();

    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_STOPPING);
    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    fake_recorder_delete_error = ESP_FAIL;
    assert(measurement_service_delete_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_REVIEW);
    assert(current.last_error == ESP_FAIL);
    assert(fake_recorder_delete_calls == 1U);
    stop_service();
}

static void test_delayed_stop_error_blocks_restart_until_cleanup(void)
{
    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();

    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_STOPPING);

    fake_recorder.last_error = ESP_FAIL;
    fake_recorder.state = SEISMIC_RECORDER_ERROR;
    fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    fake_recorder.stop_stage_error = ESP_FAIL;
    measurement_service_process_stop_poll();
    assert(snapshot().state == MEASUREMENT_SERVICE_STOPPING);
    assert(snapshot().last_error == ESP_FAIL);
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);

    assert(seismic_recorder_wait_for_stop() == ESP_FAIL);
    measurement_service_process_stop_poll();
    assert(snapshot().state == MEASUREMENT_SERVICE_ERROR);
    assert(snapshot().last_error == ESP_FAIL);
    stop_service();
}

int main(void)
{
    test_initial_and_no_active_object();
    test_start_stop_keep_delete_and_repeated_start();
    test_retry_start_after_error_keeps_gate_until_worker_runs();
    test_underlying_errors_and_queue_full();
    test_snapshot_and_delete_errors_preserve_safe_states();
    test_delayed_stop_error_blocks_restart_until_cleanup();
    return 0;
}
