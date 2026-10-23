/* Exercise the production worker handlers with boundary fakes. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "measurement_service.h"
#include "app_stop_timeout_config.h"
#include "storage_bulk_admission.h"
#include "storage_manager_internal.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../../../tests/host/host_threads.h"

typedef struct {
    size_t length;
    size_t item_size;
    bool occupied;
    unsigned char item[128];
} fake_queue_t;

typedef struct {
    UBaseType_t count;
} fake_semaphore_t;

static SemaphoreHandle_t s_mutex;
static measurement_service_snapshot_t s_snapshot;
static bool s_measurement_mutex_held;
static host_mutex_t s_queue_race_lock = HOST_MUTEX_INITIALIZER;
static host_cond_t s_queue_race_changed = HOST_COND_INITIALIZER;
static bool s_fail_queue_create;
static bool s_pause_next_queue_send;
static bool s_queue_send_paused;
static bool s_release_queue_send;
static bool s_hold_shutdown_wait;
static bool s_shutdown_waiting;
static bool s_release_shutdown_wait;
static esp_err_t s_race_submit_result;
static esp_err_t s_race_shutdown_result;

static fake_queue_t *last_queue;
static TickType_t s_last_queue_send_timeout;
static unsigned s_task_delay_calls;
static unsigned s_queue_delete_calls;
static unsigned s_semaphore_delete_calls;
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
static char fake_recorder_delete_object_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
static char fake_recorder_delete_session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
static object_service_summary_t fake_object;
static seismic_recorder_snapshot_t fake_recorder;
static void (*fake_busy_read_hook)(void);
static bool fake_busy_read_interleaved;
static bool fake_start_guard_allowed;
static unsigned fake_start_guard_calls;
static esp_err_t fake_start_object_error;
static unsigned fake_start_object_calls;
static char fake_start_object_last_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
static unsigned fake_start_release_calls;
static bool fake_start_release_saw_starting;
static esp_err_t fake_bulk_admission_enter_error;
static unsigned s_bulk_admission_enter_calls;
static unsigned s_bulk_admission_exit_calls;
static bool s_starting_state_observed;
static bool s_starting_observed_without_admission;
extern esp_err_t measurement_source_start_result;
extern unsigned measurement_source_ready_after;
extern unsigned measurement_source_snapshot_calls;
extern unsigned measurement_source_start_calls;

QueueHandle_t xQueueCreate(size_t length, size_t item_size)
{
    if (s_fail_queue_create) return NULL;
    fake_queue_t *queue = calloc(1, sizeof(*queue));
    if (queue != NULL && item_size <= sizeof(queue->item)) {
        queue->length = length;
        queue->item_size = item_size;
        last_queue = queue;
    }
    else if (queue != NULL) {
        free(queue);
        queue = NULL;
    }
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout)
{
    s_last_queue_send_timeout = timeout;
    fake_queue_t *queue = handle;
    if (queue == NULL) return pdFALSE;
    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    if (s_pause_next_queue_send) {
        s_pause_next_queue_send = false;
        s_queue_send_paused = true;
        assert(host_cond_broadcast(&s_queue_race_changed) == 0);
        while (!s_release_queue_send) {
            assert(host_cond_wait_for(
                &s_queue_race_changed, &s_queue_race_lock, 5000U) == 0);
        }
    }
    while (queue->occupied && timeout != 0U) {
        if (host_cond_wait_for(
                &s_queue_race_changed,
                &s_queue_race_lock,
                (uint32_t)timeout) != 0) {
            assert(host_mutex_unlock(&s_queue_race_lock) == 0);
            return pdFALSE;
        }
    }
    if (queue->occupied) {
        assert(host_mutex_unlock(&s_queue_race_lock) == 0);
        return pdFALSE;
    }
    memcpy(queue->item, item, queue->item_size);
    queue->occupied = true;
    assert(host_cond_broadcast(&s_queue_race_changed) == 0);
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout)
{
    fake_queue_t *queue = handle;
    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    if (queue == NULL || !queue->occupied) {
        assert(host_mutex_unlock(&s_queue_race_lock) == 0);
        return pdFALSE;
    }
    (void)timeout;
    memcpy(item, queue->item, queue->item_size);
    queue->occupied = false;
    assert(host_cond_broadcast(&s_queue_race_changed) == 0);
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t handle)
{
    if (handle != NULL) ++s_queue_delete_calls;
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
    if (handle == s_mutex) {
        s_measurement_mutex_held = true;
    }
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL) {
        return pdFALSE;
    }
    semaphore->count++;
    if (handle == s_mutex && s_snapshot.state == MEASUREMENT_SERVICE_STARTING) {
        s_starting_state_observed = true;
        if (!storage_bulk_admission_is_protected()) {
            s_starting_observed_without_admission = true;
        }
    }
    if (handle == s_mutex) {
        s_measurement_mutex_held = false;
    }
    return pdTRUE;
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t handle)
{
    const fake_semaphore_t *semaphore = handle;
    if (fake_busy_read_hook != NULL) {
        fake_busy_read_hook();
    }
    return semaphore == NULL ? 0U : semaphore->count;
}

void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    if (handle != NULL) ++s_semaphore_delete_calls;
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
    if (ticks == 1U) {
        assert(host_mutex_lock(&s_queue_race_lock) == 0);
        if (s_hold_shutdown_wait) {
            s_shutdown_waiting = true;
            assert(host_cond_broadcast(&s_queue_race_changed) == 0);
            while (!s_release_shutdown_wait) {
                assert(host_cond_wait_for(
                    &s_queue_race_changed, &s_queue_race_lock, 5000U) == 0);
            }
            s_hold_shutdown_wait = false;
        }
        assert(host_mutex_unlock(&s_queue_race_lock) == 0);
    }
    ++s_task_delay_calls;
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
    ++fake_recorder_delete_calls;
    snprintf(
        fake_recorder_delete_object_id,
        sizeof(fake_recorder_delete_object_id),
        "%s",
        object_id == NULL ? "" : object_id
    );
    snprintf(
        fake_recorder_delete_session_id,
        sizeof(fake_recorder_delete_session_id),
        "%s",
        session_id == NULL ? "" : session_id
    );
    return fake_recorder_delete_error;
}

static esp_err_t test_storage_bulk_admission_enter_protected(void)
{
    ++s_bulk_admission_enter_calls;
    assert(!s_measurement_mutex_held);
    if (fake_bulk_admission_enter_error != ESP_OK) {
        return fake_bulk_admission_enter_error;
    }
    return storage_bulk_admission_enter_protected();
}


static void test_storage_bulk_admission_exit_protected(void)
{
    ++s_bulk_admission_exit_calls;
    storage_bulk_admission_exit_protected();
}


#define storage_bulk_admission_enter_protected test_storage_bulk_admission_enter_protected
#define storage_bulk_admission_exit_protected test_storage_bulk_admission_exit_protected
#include "../measurement_service.c"
#undef storage_bulk_admission_exit_protected
#undef storage_bulk_admission_enter_protected

static void fake_try_worker_transition_during_busy_read(void)
{
    /* A real worker can only change s_snapshot while it owns s_mutex.  The
     * non-blocking take models that concurrent attempt without deadlocking
     * this single-threaded contract test. */
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return;
    }

    fake_busy_read_interleaved = true;
    s_snapshot.state = MEASUREMENT_SERVICE_RECORDING;
    xSemaphoreGive(s_mutex);
}

static void reset_fakes(void)
{
    measurement_source_start_result = ESP_OK;
    measurement_source_ready_after = 0U;
    measurement_source_snapshot_calls = 0U;
    measurement_source_start_calls = 0U;
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
    fake_recorder_delete_object_id[0] = '\0';
    fake_recorder_delete_session_id[0] = '\0';
    fake_busy_read_hook = NULL;
    fake_busy_read_interleaved = false;
    fake_start_guard_allowed = true;
    fake_start_guard_calls = 0U;
    fake_start_object_error = ESP_OK;
    fake_start_object_calls = 0U;
    fake_start_object_last_id[0] = '\0';
    fake_start_release_calls = 0U;
    fake_start_release_saw_starting = false;
    fake_bulk_admission_enter_error = ESP_OK;
    s_bulk_admission_enter_calls = 0U;
    s_bulk_admission_exit_calls = 0U;
    s_measurement_mutex_held = false;
    s_last_queue_send_timeout = 0U;
    s_task_delay_calls = 0U;
    s_starting_state_observed = false;
    s_starting_observed_without_admission = false;
}


static void set_fake_active_object(const char *object_id, const char *name)
{
    memset(&fake_object, 0, sizeof(fake_object));
    snprintf(fake_object.id, sizeof(fake_object.id), "%s", object_id);
    snprintf(fake_object.name, sizeof(fake_object.name), "%s", name);
    fake_active_object = true;
}


static esp_err_t fake_start_guard(void *user_context)
{
    (void)user_context;
    ++fake_start_guard_calls;
    return fake_start_guard_allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}


static esp_err_t fake_start_object(
    const char *object_id,
    void *user_context
)
{
    (void)user_context;
    ++fake_start_object_calls;
    snprintf(
        fake_start_object_last_id,
        sizeof(fake_start_object_last_id),
        "%s",
        object_id
    );
    if (fake_start_object_error != ESP_OK) {
        return fake_start_object_error;
    }

    snprintf(fake_object.id, sizeof(fake_object.id), "%s", object_id);
    fake_active_object = true;
    return ESP_OK;
}


static void fake_start_reservation_release(void *user_context)
{
    (void)user_context;
    ++fake_start_release_calls;
    fake_start_release_saw_starting =
        s_snapshot.state == MEASUREMENT_SERVICE_STARTING;
}

static void start_service(void)
{
    storage_bulk_admission_exit_protected();
    reset_fakes();
    assert(measurement_service_init() == ESP_OK);
    /* The fake scheduler does not run the worker task. */
    s_task = NULL;
}

static void stop_service(void)
{
    if (last_queue != NULL) last_queue->occupied = false;
    s_task = NULL;
    assert(measurement_service_shutdown() == ESP_OK);
    assert(!storage_bulk_admission_is_protected());
    last_queue = NULL;
}

static void run_pending_command(void)
{
    measurement_command_t command;
    assert(last_queue != NULL);
    assert(xQueueReceive(last_queue, &command, 0) == pdTRUE);
    switch (command.type) {
        case MEASUREMENT_COMMAND_START: measurement_service_process_start(&command); break;
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
    assert(s_bulk_admission_enter_calls == 1U);
    assert(s_bulk_admission_exit_calls == 1U);
    assert(s_starting_state_observed);
    assert(!s_starting_observed_without_admission);
    assert(!storage_bulk_admission_is_protected());
    stop_service();
}

static void test_start_stop_keep_delete_and_repeated_start(void)
{
    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    assert(measurement_service_is_busy());
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);
    run_pending_command();
    assert(s_starting_state_observed);
    assert(!s_starting_observed_without_admission);
    assert(s_bulk_admission_enter_calls == 1U);
    assert(s_bulk_admission_exit_calls == 0U);
    assert(storage_bulk_admission_is_protected());
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
    assert(s_bulk_admission_exit_calls == 1U);
    assert(!storage_bulk_admission_is_protected());

    assert(measurement_service_keep_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(current.session_id[0] == '\0');
    assert(s_snapshot.object_id[0] == '\0');
    assert(s_snapshot.object_name[0] == '\0');

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
    assert(s_snapshot.object_id[0] == '\0');
    assert(s_snapshot.object_name[0] == '\0');
    assert(s_bulk_admission_enter_calls == 2U);
    assert(s_bulk_admission_exit_calls == 2U);
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
    assert(s_bulk_admission_enter_calls == 1U);
    assert(s_bulk_admission_exit_calls == 1U);
    assert(!storage_bulk_admission_is_protected());

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


static void test_start_reservation_release_waits_for_starting_state(void)
{
    start_service();
    assert(measurement_service_set_start_guard(fake_start_guard, NULL) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        fake_start_reservation_release,
        NULL
    ) == ESP_OK);

    assert(measurement_service_start_async() == ESP_OK);
    assert(fake_start_guard_calls == 1U);
    assert(fake_start_release_calls == 0U);
    assert(measurement_service_is_busy());

    run_pending_command();
    assert(fake_start_release_calls == 1U);
    assert(fake_start_release_saw_starting);
    assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);
    stop_service();
}

static void test_object_specific_start_is_transactional(void)
{
    start_service();
    assert(measurement_service_set_start_object_callback(
        fake_start_object,
        NULL
    ) == ESP_OK);

    /* A recording for object A rejects START(B) before the object callback can
     * mutate the persistent active-object selection. */
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(strcmp(fake_object.id, "OBJ_0001") == 0);
    assert(measurement_service_start_object_async("OBJ_0002") == ESP_ERR_INVALID_STATE);
    assert(strcmp(fake_object.id, "OBJ_0001") == 0);
    assert(fake_start_object_calls == 0U);
    stop_service();

    start_service();
    assert(measurement_service_set_start_object_callback(
        fake_start_object,
        NULL
    ) == ESP_OK);
    fake_start_guard_allowed = false;
    assert(measurement_service_set_start_guard(fake_start_guard, NULL) == ESP_OK);

    /* A cache-refresh reservation rejects START(B) before the callback too. */
    assert(measurement_service_start_object_async("OBJ_0002") == ESP_ERR_INVALID_STATE);
    assert(fake_start_guard_calls == 1U);
    assert(strcmp(fake_object.id, "OBJ_0001") == 0);
    assert(fake_start_object_calls == 0U);
    assert(!measurement_service_is_busy());
    stop_service();

    start_service();
    assert(measurement_service_set_start_object_callback(
        fake_start_object,
        NULL
    ) == ESP_OK);

    /* An accepted object-specific START commits B only when the worker owns
     * the command, then starts the recorder for that committed object. */
    char caller_object_id[OBJECT_SERVICE_OBJECT_ID_SIZE] = "OBJ_0002";
    assert(measurement_service_start_object_async(caller_object_id) == ESP_OK);
    snprintf(caller_object_id, sizeof(caller_object_id), "OBJ_0003");
    assert(strcmp(fake_object.id, "OBJ_0001") == 0);
    run_pending_command();
    assert(fake_start_object_calls == 1U);
    assert(strcmp(fake_start_object_last_id, "OBJ_0002") == 0);
    assert(strcmp(fake_object.id, "OBJ_0002") == 0);
    assert(strcmp(fake_recorder.object_id, "OBJ_0002") == 0);
    assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);
    stop_service();
}


static void test_session_owner_is_immutable_across_active_switch(void)
{
    start_service();

    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();

    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.object_name, "Test object") == 0);
    assert(strcmp(fake_recorder.object_id, current.object_id) == 0);

    /* Global ACTIVE may later move to B, but the live session still belongs
     * to the object captured by START. */
    set_fake_active_object("OBJ_0002", "Later active");
    current = snapshot();
    assert(current.active_object_present);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.object_name, "Test object") == 0);
    assert(strcmp(fake_recorder.object_id, current.object_id) == 0);

    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_STOPPING);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.object_name, "Test object") == 0);

    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_REVIEW);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.object_name, "Test object") == 0);
    assert(measurement_service_snapshot_allows_object_mutation(&current));

    /* The synchronous fake cannot observe process_delete mid-call, so expose
     * the exact DELETING snapshot state directly and verify projection rules. */
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_DELETING;
    measurement_service_unlock();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_DELETING);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.object_name, "Test object") == 0);
    assert(!measurement_service_snapshot_allows_object_mutation(&current));

    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_REVIEW;
    measurement_service_unlock();

    assert(measurement_service_delete_async() == ESP_OK);
    run_pending_command();
    assert(fake_recorder_delete_calls == 1U);
    assert(strcmp(fake_recorder_delete_object_id, "OBJ_0001") == 0);
    assert(strcmp(fake_recorder_delete_session_id, "S000001") == 0);

    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(current.active_object_present);
    assert(strcmp(current.object_id, "OBJ_0002") == 0);
    assert(strcmp(current.object_name, "Later active") == 0);
    stop_service();
}


static void test_error_owner_projection_semantics(void)
{
    /* Failure before an owner is captured remains an idle retry context and
     * therefore projects whichever object is currently ACTIVE. */
    start_service();
    fake_active_object = false;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(s_snapshot.object_id[0] == '\0');
    set_fake_active_object("OBJ_0002", "Retry object");
    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.active_object_present);
    assert(strcmp(current.object_id, "OBJ_0002") == 0);
    assert(strcmp(current.object_name, "Retry object") == 0);
    stop_service();

    /* Failure after object resolution retains that concrete attempted owner,
     * even if global ACTIVE later changes. */
    start_service();
    fake_recorder_start_error = ESP_FAIL;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(s_snapshot.state == MEASUREMENT_SERVICE_ERROR);
    assert(strcmp(s_snapshot.object_id, "OBJ_0001") == 0);
    assert(s_bulk_admission_enter_calls == 1U);
    assert(s_bulk_admission_exit_calls == 1U);
    measurement_service_set_error(ESP_FAIL);
    assert(s_bulk_admission_exit_calls == 1U);
    assert(!storage_bulk_admission_is_protected());
    set_fake_active_object("OBJ_0002", "Later active");
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.active_object_present);
    assert(strcmp(current.object_id, "OBJ_0001") == 0);
    assert(strcmp(current.object_name, "Test object") == 0);
    stop_service();
}


static void test_keep_clears_owner_and_next_start_resolves_fresh_active(void)
{
    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(measurement_service_stop_async() == ESP_OK);
    run_pending_command();
    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    assert(snapshot().state == MEASUREMENT_SERVICE_REVIEW);

    set_fake_active_object("OBJ_0002", "Next object");
    assert(measurement_service_keep_async() == ESP_OK);
    run_pending_command();
    assert(s_snapshot.object_id[0] == '\0');
    assert(s_snapshot.object_name[0] == '\0');

    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(strcmp(current.object_id, "OBJ_0002") == 0);
    assert(strcmp(current.object_name, "Next object") == 0);

    assert(measurement_service_start_async() == ESP_OK);
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(current.busy);
    assert(strcmp(current.object_id, "OBJ_0002") == 0);
    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);
    assert(strcmp(current.object_id, "OBJ_0002") == 0);
    assert(strcmp(fake_recorder.object_id, "OBJ_0002") == 0);
    stop_service();
}


static void test_object_mutation_policy_table(void)
{
    static const struct {
        measurement_service_state_t state;
        bool allowed;
    } cases[] = {
        {MEASUREMENT_SERVICE_LIVE, true},
        {MEASUREMENT_SERVICE_STARTING, false},
        {MEASUREMENT_SERVICE_RECORDING, false},
        {MEASUREMENT_SERVICE_STOPPING, false},
        {MEASUREMENT_SERVICE_REVIEW, true},
        {MEASUREMENT_SERVICE_DELETING, false},
        {MEASUREMENT_SERVICE_ERROR, true},
    };

    assert(!measurement_service_snapshot_allows_object_mutation(NULL));
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        measurement_service_snapshot_t current = {
            .busy = false,
            .state = cases[index].state,
        };
        assert(
            measurement_service_snapshot_allows_object_mutation(&current) ==
            cases[index].allowed
        );
        current.busy = true;
        assert(!measurement_service_snapshot_allows_object_mutation(&current));
    }
}


static void test_acquisition_stop_cannot_bypass_recording_state(void)
{
    start_service();

    measurement_service_snapshot_t current = snapshot();
    assert(!measurement_service_snapshot_blocks_acquisition_stop(&current));

    /* START is accepted first; the queued busy window is already owned by
     * measurement_service even though the worker still reports LIVE. */
    assert(measurement_service_start_async() == ESP_OK);
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_LIVE);
    assert(current.busy);
    assert(measurement_service_snapshot_blocks_acquisition_stop(&current));

    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);
    assert(measurement_service_snapshot_blocks_acquisition_stop(&current));

    /* The only legal way out of RECORDING is the measurement STOP command. */
    assert(measurement_service_stop_async() == ESP_OK);
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_RECORDING);
    assert(measurement_service_snapshot_blocks_acquisition_stop(&current));

    run_pending_command();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_STOPPING);
    assert(measurement_service_snapshot_blocks_acquisition_stop(&current));

    assert(seismic_recorder_wait_for_stop() == ESP_OK);
    measurement_service_process_stop_poll();
    current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_REVIEW);
    assert(!measurement_service_snapshot_blocks_acquisition_stop(&current));
    stop_service();
}


static void test_start_guard_rejects_without_leaking_release(void)
{
    start_service();
    fake_start_guard_allowed = false;
    assert(measurement_service_set_start_guard(fake_start_guard, NULL) == ESP_OK);
    assert(measurement_service_set_start_reservation_release_callback(
        fake_start_reservation_release,
        NULL
    ) == ESP_OK);

    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);
    assert(fake_start_guard_calls == 1U);
    assert(fake_start_release_calls == 0U);
    assert(!measurement_service_is_busy());
    assert(s_bulk_admission_enter_calls == 0U);
    assert(s_bulk_admission_exit_calls == 0U);
    assert(!storage_bulk_admission_is_protected());
    stop_service();
}

static void test_admission_enter_failure_does_not_leak(void)
{
    start_service();
    fake_bulk_admission_enter_error = ESP_ERR_INVALID_STATE;

    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();

    assert(snapshot().state == MEASUREMENT_SERVICE_ERROR);
    assert(s_bulk_admission_enter_calls == 1U);
    assert(s_bulk_admission_exit_calls == 0U);
    assert(!s_starting_state_observed);
    assert(!storage_bulk_admission_is_protected());
    stop_service();
}

static void test_invalid_worker_start_releases_admission_once(void)
{
    start_service();
    measurement_command_t command = {
        .type = MEASUREMENT_COMMAND_START,
    };
    snprintf(command.object_id, sizeof(command.object_id), "OBJ_0002");

    measurement_service_process_start(&command);

    assert(snapshot().state == MEASUREMENT_SERVICE_ERROR);
    assert(fake_recorder_start_calls == 0U);
    assert(s_bulk_admission_enter_calls == 1U);
    assert(s_bulk_admission_exit_calls == 1U);
    assert(!s_starting_state_observed);
    assert(!storage_bulk_admission_is_protected());

    measurement_service_set_error(ESP_ERR_INVALID_STATE);
    assert(s_bulk_admission_exit_calls == 1U);
    stop_service();
}

static void test_snapshot_state_and_busy_share_one_critical_section(void)
{
    start_service();
    fake_recorder_start_error = ESP_FAIL;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();

    measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(!current.busy);

    /* If busy is read after unlocking, this hook can acquire the mutex and
     * emulate the worker completing a transition between the two reads. */
    fake_busy_read_interleaved = false;
    fake_busy_read_hook = fake_try_worker_transition_during_busy_read;
    assert(measurement_service_get_snapshot(&current) == ESP_OK);
    fake_busy_read_hook = NULL;

    assert(!fake_busy_read_interleaved);
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
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
    measurement_service_queue_stats_t queue_stats;
    assert(measurement_service_get_queue_stats(&queue_stats) == ESP_OK);
    assert(queue_stats.peak_occupancy == 1U && queue_stats.overflow_count == 1U);
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


static void test_full_queue_stop_then_recorder_fault_never_reports_stopped_ok(void)
{
    start_service();
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);

    const measurement_command_t queued_stop = {
        .type = MEASUREMENT_COMMAND_STOP,
    };
    assert(xQueueSend(last_queue, &queued_stop, 0U) == pdTRUE);
    assert(xSemaphoreGive(s_gate) == pdTRUE);
    assert(measurement_service_stop_async() == ESP_ERR_TIMEOUT);

    fake_recorder.running = false;
    fake_recorder.state = SEISMIC_RECORDER_ERROR;
    fake_recorder.last_error = ESP_FAIL;
    fake_recorder.stop_stage_error = ESP_FAIL;
    fake_recorder.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    measurement_service_process_stop_poll();

    const measurement_service_snapshot_t current = snapshot();
    assert(current.state == MEASUREMENT_SERVICE_ERROR);
    assert(current.last_error == ESP_FAIL);
    assert(current.stop_stage != SEISMIC_RECORDER_STOP_STAGE_COMPLETE);
    assert(current.state != MEASUREMENT_SERVICE_REVIEW);
    measurement_service_queue_stats_t queue_stats;
    assert(measurement_service_get_queue_stats(&queue_stats) == ESP_OK);
    assert(queue_stats.peak_occupancy == 1U && queue_stats.overflow_count == 1U);
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
    assert(strcmp(fake_recorder_delete_object_id, "OBJ_0001") == 0);
    assert(strcmp(fake_recorder_delete_session_id, "S000001") == 0);
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
    assert(s_bulk_admission_exit_calls == 1U);
    assert(!storage_bulk_admission_is_protected());
    stop_service();
}

static void test_shutdown_timeout_retains_worker_owned_resources(void)
{
    start_service();
    s_task = (TaskHandle_t)(uintptr_t)1U;
    const unsigned queues_deleted_before = s_queue_delete_calls;
    const unsigned semaphores_deleted_before = s_semaphore_delete_calls;
    const TickType_t timeout_ticks = pdMS_TO_TICKS(APP_STOP_TIMEOUT_WORKER_MS);

    assert(measurement_service_shutdown() == ESP_ERR_TIMEOUT);
    assert(s_last_queue_send_timeout == timeout_ticks);
    assert(s_task_delay_calls == timeout_ticks);
    assert(s_task != NULL);
    assert(s_queue != NULL && s_mutex != NULL && s_gate != NULL);
    assert(s_queue_delete_calls == queues_deleted_before);
    assert(s_semaphore_delete_calls == semaphores_deleted_before);

    last_queue->occupied = false;
    s_task = NULL;
    assert(measurement_service_shutdown() == ESP_OK);
    assert(s_queue_delete_calls == queues_deleted_before + 1U);
    assert(s_semaphore_delete_calls == semaphores_deleted_before + 2U);
}


static void *race_submit_start(void *unused)
{
    (void)unused;
    s_race_submit_result = measurement_service_start_async();
    return NULL;
}


static void *race_shutdown(void *unused)
{
    (void)unused;
    s_race_shutdown_result = measurement_service_shutdown();
    return NULL;
}


static void test_submit_finishes_before_queue_teardown(void)
{
    start_service();
    assert(last_queue->length == 1U);
    assert(last_queue->item_size == sizeof(measurement_command_t));

    measurement_command_t untouched = {
        .type = MEASUREMENT_COMMAND_STOP,
        .object_id = "unchanged",
    };
    assert(xQueueReceive(last_queue, &untouched, 0U) == pdFALSE);
    assert(untouched.type == MEASUREMENT_COMMAND_STOP);
    assert(strcmp(untouched.object_id, "unchanged") == 0);

    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    s_pause_next_queue_send = true;
    s_queue_send_paused = false;
    s_release_queue_send = false;
    s_hold_shutdown_wait = true;
    s_shutdown_waiting = false;
    s_release_shutdown_wait = false;
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);

    host_thread_t producer;
    host_thread_t shutdown;
    assert(host_thread_create(&producer, race_submit_start, NULL) == 0);
    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    while (!s_queue_send_paused) {
        assert(host_cond_wait_for(
            &s_queue_race_changed, &s_queue_race_lock, 5000U) == 0);
    }
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);

    const unsigned deletes_before = s_queue_delete_calls;
    assert(host_thread_create(&shutdown, race_shutdown, NULL) == 0);
    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    while (!s_shutdown_waiting) {
        assert(host_cond_wait_for(
            &s_queue_race_changed, &s_queue_race_lock, 5000U) == 0);
    }
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);
    assert(s_queue_delete_calls == deletes_before);

    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    s_release_queue_send = true;
    assert(host_cond_broadcast(&s_queue_race_changed) == 0);
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);
    assert(host_thread_join(producer) == 0);
    assert(s_race_submit_result == ESP_OK);
    assert(last_queue->occupied);
    run_pending_command();
    assert(fake_recorder_start_calls == 1U && fake_recorder.running);

    assert(host_mutex_lock(&s_queue_race_lock) == 0);
    s_release_shutdown_wait = true;
    assert(host_cond_broadcast(&s_queue_race_changed) == 0);
    assert(host_mutex_unlock(&s_queue_race_lock) == 0);
    assert(host_thread_join(shutdown) == 0);

    assert(s_race_shutdown_result == ESP_OK);
    assert(s_queue_delete_calls == deletes_before + 1U);
    assert(measurement_service_start_async() == ESP_ERR_INVALID_STATE);
    last_queue = NULL;
}


static void test_queue_creation_failure_has_no_partial_service(void)
{
    const unsigned queues_deleted_before = s_queue_delete_calls;
    const unsigned semaphores_deleted_before = s_semaphore_delete_calls;
    last_queue = NULL;
    s_fail_queue_create = true;
    assert(measurement_service_init() == ESP_ERR_NO_MEM);
    s_fail_queue_create = false;
    assert(last_queue == NULL);
    assert(s_queue_delete_calls == queues_deleted_before);
    assert(s_semaphore_delete_calls == semaphores_deleted_before);
    assert(!measurement_service_is_busy());
}

static void test_source_warmup_is_bounded_and_precedes_recorder_start(void)
{
    start_service();
    measurement_source_ready_after = 3U;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_RECORDING);
    assert(measurement_source_start_calls == 1U);
    assert(measurement_source_snapshot_calls == 4U);
    assert(s_task_delay_calls == 3U);
    assert(fake_recorder_start_calls == 1U);
    stop_service();

    start_service();
    measurement_source_ready_after = UINT32_MAX;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().state == MEASUREMENT_SERVICE_ERROR);
    assert(snapshot().last_error == ESP_ERR_TIMEOUT);
    assert(fake_recorder_start_calls == 0U);
    assert(s_task_delay_calls == pdMS_TO_TICKS(1000U));
    assert(!storage_bulk_admission_is_protected());
    stop_service();

    start_service();
    measurement_source_start_result = ESP_FAIL;
    assert(measurement_service_start_async() == ESP_OK);
    run_pending_command();
    assert(snapshot().last_error == ESP_FAIL);
    assert(fake_recorder_start_calls == 0U);
    assert(measurement_source_snapshot_calls == 0U);
    stop_service();
}

int main(void)
{
    assert(storage_bulk_admission_initialize() == ESP_OK);
    test_queue_creation_failure_has_no_partial_service();
    test_initial_and_no_active_object();
    test_source_warmup_is_bounded_and_precedes_recorder_start();
    test_start_stop_keep_delete_and_repeated_start();
    test_retry_start_after_error_keeps_gate_until_worker_runs();
    test_start_reservation_release_waits_for_starting_state();
    test_object_specific_start_is_transactional();
    test_session_owner_is_immutable_across_active_switch();
    test_error_owner_projection_semantics();
    test_keep_clears_owner_and_next_start_resolves_fresh_active();
    test_object_mutation_policy_table();
    test_acquisition_stop_cannot_bypass_recording_state();
    test_start_guard_rejects_without_leaking_release();
    test_admission_enter_failure_does_not_leak();
    test_invalid_worker_start_releases_admission_once();
    test_snapshot_state_and_busy_share_one_critical_section();
    test_full_queue_stop_then_recorder_fault_never_reports_stopped_ok();
    test_underlying_errors_and_queue_full();
    test_snapshot_and_delete_errors_preserve_safe_states();
    test_delayed_stop_error_blocks_restart_until_cleanup();
    test_shutdown_timeout_retains_worker_owned_resources();
    test_submit_finishes_before_queue_teardown();
    return 0;
}
