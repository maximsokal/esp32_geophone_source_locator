#include "measurement_service.h"

#include <stdio.h>
#include <string.h>

#include "app_task_config.h"
#include "object_format.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "runtime_observability.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


#define MEASUREMENT_SERVICE_QUEUE_LENGTH  1


typedef enum
{
    MEASUREMENT_COMMAND_START = 0,
    MEASUREMENT_COMMAND_STOP,
    MEASUREMENT_COMMAND_KEEP,
    MEASUREMENT_COMMAND_DELETE,
    MEASUREMENT_COMMAND_SHUTDOWN

} measurement_command_type_t;


typedef enum
{
    MEASUREMENT_HOOK_START_GUARD = 0,
    MEASUREMENT_HOOK_START_OBJECT,
    MEASUREMENT_HOOK_START_RESERVATION_RELEASE,
    MEASUREMENT_HOOK_STORAGE_REFRESH,
    MEASUREMENT_HOOK_COUNT

} measurement_hook_type_t;


typedef struct
{
    measurement_command_type_t type;
    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
    measurement_service_start_object_callback_t start_object_callback;
    void *start_object_context;
    bool start_object_hook_owned;

} measurement_command_t;


static const char *TAG = "MEASUREMENT_SERVICE";

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_gate = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static measurement_service_snapshot_t s_snapshot;
static bool s_initialized = false;

/*
 * Hook pointers are shared by lifecycle/Web tasks and the measurement worker.
 * The spinlock protects pointer/context snapshots and lifetime counters only.
 * User callbacks, waits and FreeRTOS blocking APIs always run outside it.
 */
static portMUX_TYPE s_hook_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_hook_in_flight[MEASUREMENT_HOOK_COUNT];

static measurement_service_start_guard_callback_t s_start_guard = NULL;
static void *s_start_guard_context = NULL;
static measurement_service_start_object_callback_t s_start_object_callback = NULL;
static void *s_start_object_context = NULL;
static measurement_service_start_reservation_release_callback_t
    s_start_reservation_release_callback = NULL;
static void *s_start_reservation_release_context = NULL;
static measurement_service_storage_refresh_callback_t s_storage_refresh_callback = NULL;
static void *s_storage_refresh_context = NULL;

/*
 * A successful START guard owns one exact release callback until the
 * reservation is released. These fields are protected by s_mutex; the
 * callback's lifetime itself is accounted by s_hook_in_flight.
 */
static bool s_start_reservation_held = false;
static measurement_service_start_reservation_release_callback_t
    s_start_reservation_callback = NULL;
static void *s_start_reservation_context = NULL;
static bool s_start_reservation_hook_owned = false;


static void measurement_service_log_stack_hwm(
    const char *stage
)
{
    ESP_LOGI(
        TAG,
        "STACK_HWM task=measurement_service stage=%s free=%u margin=%u",
        stage,
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        (unsigned)APP_TASK_STACK_MEASUREMENT_SERVICE_HWM_SAFETY_MARGIN_BYTES
    );
}


static void measurement_service_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}


static void measurement_service_unlock(void)
{
    xSemaphoreGive(s_mutex);
}


static unsigned measurement_service_hook_in_flight(
    measurement_hook_type_t hook
)
{
    unsigned in_flight;

    taskENTER_CRITICAL(&s_hook_lock);
    in_flight = s_hook_in_flight[hook];
    taskEXIT_CRITICAL(&s_hook_lock);

    return in_flight;
}


static void measurement_service_hook_release(
    measurement_hook_type_t hook
)
{
    taskENTER_CRITICAL(&s_hook_lock);
    if (s_hook_in_flight[hook] > 0U) {
        --s_hook_in_flight[hook];
    }
    taskEXIT_CRITICAL(&s_hook_lock);
}


static void measurement_service_wait_for_hook(
    measurement_hook_type_t hook
)
{
    while (measurement_service_hook_in_flight(hook) != 0U) {
        /* Never block or yield while holding the SMP spinlock. */
        vTaskDelay(1);
    }
}


static void measurement_service_release_gate(void)
{
    if (s_gate != NULL) {
        (void)xSemaphoreGive(s_gate);
    }
}


static void measurement_service_release_start_object_command(
    measurement_command_t *command
)
{
    if (command == NULL || !command->start_object_hook_owned) {
        return;
    }

    command->start_object_hook_owned = false;
    measurement_service_hook_release(MEASUREMENT_HOOK_START_OBJECT);
}


static void measurement_service_release_start_reservation(void)
{
    measurement_service_start_reservation_release_callback_t callback = NULL;
    void *user_context = NULL;
    bool hook_owned = false;

    measurement_service_lock();
    if (s_start_reservation_held) {
        s_start_reservation_held = false;
        callback = s_start_reservation_callback;
        user_context = s_start_reservation_context;
        hook_owned = s_start_reservation_hook_owned;
        s_start_reservation_callback = NULL;
        s_start_reservation_context = NULL;
        s_start_reservation_hook_owned = false;
    }
    measurement_service_unlock();

    if (callback != NULL) {
        callback(user_context);
    }

    if (hook_owned) {
        measurement_service_hook_release(
            MEASUREMENT_HOOK_START_RESERVATION_RELEASE
        );
    }
}


static void measurement_service_clear_session_identity_locked(void)
{
    s_snapshot.object_id[0] = '\0';
    s_snapshot.object_name[0] = '\0';
    s_snapshot.session_id[0] = '\0';
}


static void measurement_service_set_error(
    esp_err_t error
)
{
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_ERROR;
    s_snapshot.last_error = error;
    measurement_service_unlock();
}


static void measurement_service_copy_recorder_snapshot(
    const seismic_recorder_snapshot_t *recorder
)
{
    if (recorder == NULL) {
        return;
    }

    measurement_service_lock();

    snprintf(
        s_snapshot.object_id,
        sizeof(s_snapshot.object_id),
        "%s",
        recorder->object_id
    );

    snprintf(
        s_snapshot.session_id,
        sizeof(s_snapshot.session_id),
        "%s",
        recorder->session_id
    );

    s_snapshot.stop_stage = recorder->stop_stage;
    s_snapshot.stop_error_stage = recorder->stop_error_stage;
    s_snapshot.samples_written = recorder->samples_written;
    s_snapshot.dropped_frames = recorder->dropped_frames;
    s_snapshot.recording_ms = recorder->recording_ms;
    s_snapshot.stop_stage_error = recorder->stop_stage_error;

    measurement_service_unlock();
}


static void measurement_service_process_start(
    measurement_command_t *command
)
{
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_STARTING;
    s_snapshot.last_error = ESP_OK;
    s_snapshot.stop_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_snapshot.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_snapshot.stop_stage_error = ESP_OK;
    measurement_service_clear_session_identity_locked();
    s_snapshot.samples_written = 0U;
    s_snapshot.dropped_frames = 0U;
    s_snapshot.recording_ms = 0U;
    measurement_service_unlock();

    const bool has_requested_object =
        command != NULL && command->object_id[0] != '\0';
    if (has_requested_object) {
        if (command->start_object_callback == NULL) {
            measurement_service_release_start_object_command(command);
            measurement_service_set_error(ESP_ERR_INVALID_STATE);
            measurement_service_release_start_reservation();
            return;
        }

        const esp_err_t select_err = command->start_object_callback(
            command->object_id,
            command->start_object_context
        );
        measurement_service_release_start_object_command(command);

        if (select_err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Active object selection failed object=%s err=%s",
                command->object_id,
                esp_err_to_name(select_err)
            );
            measurement_service_set_error(select_err);
            measurement_service_release_start_reservation();
            return;
        }
    }

    /* The state is now STARTING, so a cache refresh can no longer mistake
     * this accepted START for an idle LIVE window. Keep the reservation until
     * after an object-specific START has committed its active-object change. */
    measurement_service_release_start_reservation();

    object_service_summary_t object;
    esp_err_t err = object_service_get_active(&object);
    if (err != ESP_OK || !object.active || object.id[0] == '\0') {
        measurement_service_set_error(
            err == ESP_OK
                ? ESP_ERR_NOT_FOUND
                : err
        );
        return;
    }

    measurement_service_log_stack_hwm("after-object");

    measurement_service_lock();
    snprintf(
        s_snapshot.object_id,
        sizeof(s_snapshot.object_id),
        "%s",
        object.id
    );
    snprintf(
        s_snapshot.object_name,
        sizeof(s_snapshot.object_name),
        "%s",
        object.name
    );
    measurement_service_unlock();

    ESP_LOGI(
        TAG,
        "Starting recording object=%s",
        object.id
    );

    measurement_service_log_stack_hwm("before-start");
    err = seismic_recorder_start(object.id);
    measurement_service_log_stack_hwm("after-recorder-start");
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Recording start failed object=%s err=%s",
            object.id,
            esp_err_to_name(err)
        );
        measurement_service_set_error(err);
        return;
    }

    seismic_recorder_snapshot_t recorder;
    err = seismic_recorder_get_snapshot(&recorder);
    if (err != ESP_OK) {
        esp_err_t cleanup_err = seismic_recorder_stop();
        if (cleanup_err == ESP_OK) {
            cleanup_err = seismic_recorder_wait_for_stop();
        }
        measurement_service_set_error(
            cleanup_err != ESP_OK
                ? cleanup_err
                : err
        );
        return;
    }

    measurement_service_copy_recorder_snapshot(&recorder);

    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_RECORDING;
    s_snapshot.last_error = ESP_OK;
    measurement_service_unlock();

    ESP_LOGI(
        TAG,
        "Recording started state=RECORDING object=%s session=%s",
        object.id,
        recorder.session_id
    );

    runtime_observability_request_snapshot("recording-active");
}


static void measurement_service_notify_storage_refresh(void)
{
    measurement_service_storage_refresh_callback_t callback = NULL;
    void *user_context = NULL;

    taskENTER_CRITICAL(&s_hook_lock);
    if (s_storage_refresh_callback != NULL) {
        callback = s_storage_refresh_callback;
        user_context = s_storage_refresh_context;
        ++s_hook_in_flight[MEASUREMENT_HOOK_STORAGE_REFRESH];
    }
    taskEXIT_CRITICAL(&s_hook_lock);

    if (callback == NULL) {
        return;
    }

    callback(user_context);
    measurement_service_hook_release(MEASUREMENT_HOOK_STORAGE_REFRESH);
}


static void measurement_service_process_stop_poll(void)
{
    measurement_service_lock();
    const bool stopping =
        s_snapshot.state == MEASUREMENT_SERVICE_STOPPING;
    measurement_service_unlock();

    if (!stopping) {
        return;
    }

    seismic_recorder_snapshot_t recorder;
    const esp_err_t snapshot_err =
        seismic_recorder_get_snapshot(&recorder);

    if (snapshot_err != ESP_OK) {
        measurement_service_set_error(snapshot_err);
        return;
    }

    measurement_service_copy_recorder_snapshot(&recorder);

    if (
        recorder.last_error != ESP_OK ||
        recorder.state == SEISMIC_RECORDER_ERROR
    ) {
        const esp_err_t recorder_error =
            recorder.last_error != ESP_OK
                ? recorder.last_error
                : ESP_FAIL;
        const bool recorder_still_finishing =
            recorder.running ||
            recorder.state == SEISMIC_RECORDER_STOPPING;
        measurement_service_lock();
        s_snapshot.last_error = recorder_error;
        if (!recorder_still_finishing) {
            s_snapshot.state = MEASUREMENT_SERVICE_ERROR;
        }
        measurement_service_unlock();
        return;
    }

    if (
        recorder.running ||
        recorder.state == SEISMIC_RECORDER_STOPPING
    ) {
        return;
    }

    if (recorder.state != SEISMIC_RECORDER_IDLE) {
        measurement_service_set_error(ESP_ERR_INVALID_STATE);
        return;
    }

    if (recorder.stop_stage != SEISMIC_RECORDER_STOP_STAGE_COMPLETE) {
        measurement_service_set_error(
            recorder.stop_stage_error != ESP_OK
                ? recorder.stop_stage_error
                : ESP_ERR_INVALID_STATE
        );
        return;
    }

    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_REVIEW;
    s_snapshot.last_error = ESP_OK;
    measurement_service_unlock();

    measurement_service_notify_storage_refresh();

    measurement_service_log_stack_hwm("after-stop");
    runtime_observability_request_snapshot("after-recorder-finalization");
    ESP_LOGI(
        TAG,
        "Recording stopped state=REVIEW session=%s samples=%llu drops=%llu",
        recorder.session_id,
        (unsigned long long)recorder.samples_written,
        (unsigned long long)recorder.dropped_frames
    );
}


static void measurement_service_process_stop(void)
{
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_STOPPING;
    s_snapshot.last_error = ESP_OK;
    measurement_service_unlock();

    esp_err_t err = seismic_recorder_stop();

    if (err != ESP_OK) {
        measurement_service_set_error(err);
        return;
    }

    /* STOP is an asynchronous request; completion is observed by the worker. */
    measurement_service_process_stop_poll();
}


static void measurement_service_process_keep(void)
{
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_LIVE;
    s_snapshot.last_error = ESP_OK;
    measurement_service_clear_session_identity_locked();
    s_snapshot.samples_written = 0U;
    s_snapshot.dropped_frames = 0U;
    s_snapshot.recording_ms = 0U;
    measurement_service_unlock();
}


static void measurement_service_process_delete(void)
{
    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];

    measurement_service_lock();
    snprintf(object_id, sizeof(object_id), "%s", s_snapshot.object_id);
    snprintf(session_id, sizeof(session_id), "%s", s_snapshot.session_id);
    s_snapshot.state = MEASUREMENT_SERVICE_DELETING;
    s_snapshot.last_error = ESP_OK;
    measurement_service_unlock();

    const esp_err_t err = seismic_recorder_delete_session(
        object_id,
        session_id
    );

    if (err != ESP_OK) {
        measurement_service_lock();
        s_snapshot.state = MEASUREMENT_SERVICE_REVIEW;
        s_snapshot.last_error = err;
        measurement_service_unlock();
        return;
    }

    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_LIVE;
    s_snapshot.last_error = ESP_OK;
    measurement_service_clear_session_identity_locked();
    s_snapshot.samples_written = 0U;
    s_snapshot.dropped_frames = 0U;
    s_snapshot.recording_ms = 0U;
    measurement_service_unlock();

    measurement_service_notify_storage_refresh();

    ESP_LOGI(
        TAG,
        "Deleted session %s/%s",
        object_id,
        session_id
    );
}


static void measurement_service_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Worker started core=%d",
        APP_TASK_CORE_SERVICES
    );
    measurement_service_log_stack_hwm("idle");

    for (;;) {
        measurement_command_t command;
        measurement_service_lock();
        const bool stopping =
            s_snapshot.state == MEASUREMENT_SERVICE_STOPPING;
        measurement_service_unlock();

        if (
            xQueueReceive(
                s_queue,
                &command,
                stopping
                    ? pdMS_TO_TICKS(100)
                    : portMAX_DELAY
            ) != pdTRUE
        ) {
            if (stopping) {
                measurement_service_process_stop_poll();
            }
            continue;
        }

        if (command.type == MEASUREMENT_COMMAND_SHUTDOWN) {
            break;
        }

        switch (command.type) {
            case MEASUREMENT_COMMAND_START:
                measurement_service_process_start(&command);
                break;

            case MEASUREMENT_COMMAND_STOP:
                measurement_service_process_stop();
                break;

            case MEASUREMENT_COMMAND_KEEP:
                measurement_service_process_keep();
                break;

            case MEASUREMENT_COMMAND_DELETE:
                measurement_service_process_delete();
                break;

            case MEASUREMENT_COMMAND_SHUTDOWN:
            default:
                break;
        }

        measurement_service_release_gate();
        measurement_service_log_stack_hwm("idle");
    }

    ESP_LOGI(TAG, "Worker stopped");

    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}


static bool measurement_service_command_allowed_locked(
    measurement_command_type_t command
)
{
    switch (command) {
        case MEASUREMENT_COMMAND_START:
            return
                s_snapshot.state == MEASUREMENT_SERVICE_LIVE ||
                s_snapshot.state == MEASUREMENT_SERVICE_ERROR;

        case MEASUREMENT_COMMAND_STOP:
            return s_snapshot.state == MEASUREMENT_SERVICE_RECORDING;

        case MEASUREMENT_COMMAND_KEEP:
        case MEASUREMENT_COMMAND_DELETE:
            return s_snapshot.state == MEASUREMENT_SERVICE_REVIEW;

        case MEASUREMENT_COMMAND_SHUTDOWN:
        default:
            return false;
    }
}


static esp_err_t measurement_service_submit(
    measurement_command_type_t command_type,
    const char *start_object_id
)
{
    if (
        start_object_id != NULL &&
        !object_format_id_is_valid(start_object_id)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    measurement_command_t command = {
        .type = command_type,
    };
    if (start_object_id != NULL) {
        snprintf(
            command.object_id,
            sizeof(command.object_id),
            "%s",
            start_object_id
        );
    }

    if (
        !s_initialized ||
        s_queue == NULL ||
        s_gate == NULL ||
        s_mutex == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_gate, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    measurement_service_lock();
    const bool allowed =
        measurement_service_command_allowed_locked(command.type);
    measurement_service_unlock();

    if (!allowed) {
        measurement_service_release_gate();
        return ESP_ERR_INVALID_STATE;
    }

    measurement_service_start_guard_callback_t start_guard = NULL;
    void *start_guard_context = NULL;
    bool start_guard_hook_owned = false;

    measurement_service_start_reservation_release_callback_t
        reservation_release = NULL;
    void *reservation_release_context = NULL;
    bool reservation_release_hook_owned = false;

    if (command_type == MEASUREMENT_COMMAND_START) {
        taskENTER_CRITICAL(&s_hook_lock);

        if (s_start_guard != NULL) {
            start_guard = s_start_guard;
            start_guard_context = s_start_guard_context;
            ++s_hook_in_flight[MEASUREMENT_HOOK_START_GUARD];
            start_guard_hook_owned = true;

            /*
             * Capture the exact release hook together with the guard. If the
             * guard succeeds, this ownership transfers to the reservation.
             */
            reservation_release = s_start_reservation_release_callback;
            reservation_release_context = s_start_reservation_release_context;
            if (reservation_release != NULL) {
                ++s_hook_in_flight[
                    MEASUREMENT_HOOK_START_RESERVATION_RELEASE
                ];
                reservation_release_hook_owned = true;
            }
        }

        if (
            command.object_id[0] != '\0' &&
            s_start_object_callback != NULL
        ) {
            command.start_object_callback = s_start_object_callback;
            command.start_object_context = s_start_object_context;
            command.start_object_hook_owned = true;
            ++s_hook_in_flight[MEASUREMENT_HOOK_START_OBJECT];
        }

        taskEXIT_CRITICAL(&s_hook_lock);
    }

    if (
        command.object_id[0] != '\0' &&
        command.start_object_callback == NULL
    ) {
        if (start_guard_hook_owned) {
            measurement_service_hook_release(MEASUREMENT_HOOK_START_GUARD);
        }
        if (reservation_release_hook_owned) {
            measurement_service_hook_release(
                MEASUREMENT_HOOK_START_RESERVATION_RELEASE
            );
        }
        measurement_service_release_start_object_command(&command);
        measurement_service_release_gate();
        return ESP_ERR_INVALID_STATE;
    }

    if (start_guard != NULL) {
        const esp_err_t guard_err = start_guard(start_guard_context);

        if (start_guard_hook_owned) {
            measurement_service_hook_release(MEASUREMENT_HOOK_START_GUARD);
            start_guard_hook_owned = false;
        }

        if (guard_err != ESP_OK) {
            measurement_service_release_start_object_command(&command);
            if (reservation_release_hook_owned) {
                measurement_service_hook_release(
                    MEASUREMENT_HOOK_START_RESERVATION_RELEASE
                );
            }
            measurement_service_release_gate();
            return guard_err;
        }

        measurement_service_lock();
        s_start_reservation_held = true;
        s_start_reservation_callback = reservation_release;
        s_start_reservation_context = reservation_release_context;
        s_start_reservation_hook_owned = reservation_release_hook_owned;
        measurement_service_unlock();

        reservation_release_hook_owned = false;
    }

    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        measurement_service_release_start_object_command(&command);
        if (start_guard_hook_owned) {
            measurement_service_hook_release(MEASUREMENT_HOOK_START_GUARD);
        }
        if (reservation_release_hook_owned) {
            measurement_service_hook_release(
                MEASUREMENT_HOOK_START_RESERVATION_RELEASE
            );
        }
        if (start_guard != NULL) {
            measurement_service_release_start_reservation();
        }
        measurement_service_release_gate();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


static void measurement_service_reset_hooks(void)
{
    taskENTER_CRITICAL(&s_hook_lock);
    s_start_guard = NULL;
    s_start_guard_context = NULL;
    s_start_object_callback = NULL;
    s_start_object_context = NULL;
    s_start_reservation_release_callback = NULL;
    s_start_reservation_release_context = NULL;
    s_storage_refresh_callback = NULL;
    s_storage_refresh_context = NULL;
    for (size_t i = 0U; i < MEASUREMENT_HOOK_COUNT; ++i) {
        s_hook_in_flight[i] = 0U;
    }
    taskEXIT_CRITICAL(&s_hook_lock);
}


esp_err_t measurement_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(
        MEASUREMENT_SERVICE_QUEUE_LENGTH,
        sizeof(measurement_command_t)
    );
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_gate = xSemaphoreCreateBinary();
    if (s_gate == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        vSemaphoreDelete(s_gate);
        vQueueDelete(s_queue);
        s_gate = NULL;
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    measurement_service_reset_hooks();
    s_start_reservation_held = false;
    s_start_reservation_callback = NULL;
    s_start_reservation_context = NULL;
    s_start_reservation_hook_owned = false;
    s_snapshot.initialized = true;
    s_snapshot.state = MEASUREMENT_SERVICE_LIVE;
    s_snapshot.last_error = ESP_OK;

    (void)xSemaphoreGive(s_gate);

    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        measurement_service_task,
        "measurement_service",
        APP_TASK_STACK_MEASUREMENT_SERVICE,
        NULL,
        APP_TASK_PRIORITY_MEASUREMENT_SERVICE,
        &s_task,
        APP_TASK_CORE_SERVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (result != pdPASS) {
        vSemaphoreDelete(s_mutex);
        vSemaphoreDelete(s_gate);
        vQueueDelete(s_queue);
        s_mutex = NULL;
        s_gate = NULL;
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized core=%d stack=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES,
        APP_TASK_STACK_MEASUREMENT_SERVICE
    );
    return ESP_OK;
}


void measurement_service_shutdown(void)
{
    if (!s_initialized) {
        return;
    }

    /*
     * Close every externally owned hook while the worker is still alive.
     * Setters are lifetime barriers, so queued/snapshotted callbacks finish
     * before their owner can tear down Web/cache state.
     */
    (void)measurement_service_set_start_guard(NULL, NULL);
    (void)measurement_service_set_start_object_callback(NULL, NULL);
    (void)measurement_service_set_storage_refresh_callback(NULL, NULL);
    (void)measurement_service_set_start_reservation_release_callback(NULL, NULL);

    const measurement_command_t command = {
        .type = MEASUREMENT_COMMAND_SHUTDOWN,
    };

    if (s_queue != NULL) {
        (void)xQueueSend(
            s_queue,
            &command,
            portMAX_DELAY
        );
    }

    while (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    measurement_service_release_start_reservation();

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    if (s_gate != NULL) {
        vSemaphoreDelete(s_gate);
        s_gate = NULL;
    }

    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    measurement_service_reset_hooks();
    s_start_reservation_held = false;
    s_start_reservation_callback = NULL;
    s_start_reservation_context = NULL;
    s_start_reservation_hook_owned = false;
    s_initialized = false;

    ESP_LOGI(TAG, "Shutdown complete");
}


esp_err_t measurement_service_set_start_guard(
    measurement_service_start_guard_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_hook_lock);
    s_start_guard = NULL;
    s_start_guard_context = NULL;
    taskEXIT_CRITICAL(&s_hook_lock);

    measurement_service_wait_for_hook(MEASUREMENT_HOOK_START_GUARD);

    if (callback != NULL) {
        taskENTER_CRITICAL(&s_hook_lock);
        s_start_guard_context = user_context;
        s_start_guard = callback;
        taskEXIT_CRITICAL(&s_hook_lock);
    }

    return ESP_OK;
}


esp_err_t measurement_service_set_start_object_callback(
    measurement_service_start_object_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_hook_lock);
    s_start_object_callback = NULL;
    s_start_object_context = NULL;
    taskEXIT_CRITICAL(&s_hook_lock);

    measurement_service_wait_for_hook(MEASUREMENT_HOOK_START_OBJECT);

    if (callback != NULL) {
        taskENTER_CRITICAL(&s_hook_lock);
        s_start_object_context = user_context;
        s_start_object_callback = callback;
        taskEXIT_CRITICAL(&s_hook_lock);
    }

    return ESP_OK;
}


esp_err_t measurement_service_set_start_reservation_release_callback(
    measurement_service_start_reservation_release_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_hook_lock);
    s_start_reservation_release_callback = NULL;
    s_start_reservation_release_context = NULL;
    taskEXIT_CRITICAL(&s_hook_lock);

    measurement_service_wait_for_hook(
        MEASUREMENT_HOOK_START_RESERVATION_RELEASE
    );

    if (callback != NULL) {
        taskENTER_CRITICAL(&s_hook_lock);
        s_start_reservation_release_context = user_context;
        s_start_reservation_release_callback = callback;
        taskEXIT_CRITICAL(&s_hook_lock);
    }

    return ESP_OK;
}


esp_err_t measurement_service_set_storage_refresh_callback(
    measurement_service_storage_refresh_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_hook_lock);
    s_storage_refresh_callback = NULL;
    s_storage_refresh_context = NULL;
    taskEXIT_CRITICAL(&s_hook_lock);

    measurement_service_wait_for_hook(MEASUREMENT_HOOK_STORAGE_REFRESH);

    if (callback != NULL) {
        taskENTER_CRITICAL(&s_hook_lock);
        s_storage_refresh_context = user_context;
        s_storage_refresh_callback = callback;
        taskEXIT_CRITICAL(&s_hook_lock);
    }

    return ESP_OK;
}


esp_err_t measurement_service_start_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_START, NULL);
}


esp_err_t measurement_service_start_object_async(
    const char *object_id
)
{
    return measurement_service_submit(
        MEASUREMENT_COMMAND_START,
        object_id
    );
}


esp_err_t measurement_service_stop_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_STOP, NULL);
}


esp_err_t measurement_service_keep_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_KEEP, NULL);
}


esp_err_t measurement_service_delete_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_DELETE, NULL);
}


esp_err_t measurement_service_get_snapshot(
    measurement_service_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_mutex == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Keep the service state and submit gate in one critical section. A
     * retry START is accepted while the previous state is ERROR, so reading
     * busy after releasing the mutex could combine that stale ERROR with a
     * later worker transition and expose an impossible snapshot.
     */
    measurement_service_lock();
    *snapshot = s_snapshot;
    snapshot->busy = measurement_service_is_busy();
    measurement_service_unlock();

    const bool session_owner_present = snapshot->object_id[0] != '\0';
    const bool project_current_active =
        snapshot->state == MEASUREMENT_SERVICE_LIVE ||
        (snapshot->state == MEASUREMENT_SERVICE_ERROR && !session_owner_present);

    object_service_summary_t object;
    const esp_err_t object_err = object_service_get_active(&object);
    if (object_err == ESP_OK && object.active && object.id[0] != '\0') {
        snapshot->active_object_present = true;
        if (project_current_active) {
            snprintf(
                snapshot->object_id,
                sizeof(snapshot->object_id),
                "%s",
                object.id
            );
            snprintf(
                snapshot->object_name,
                sizeof(snapshot->object_name),
                "%s",
                object.name
            );
        }
    }
    else {
        snapshot->active_object_present = false;
        if (project_current_active) {
            snapshot->object_id[0] = '\0';
            snapshot->object_name[0] = '\0';
        }
    }

    /*
     * Do not ask the recorder for its mutex while START is doing SD work.
     * The measurement screen refresh runs with the LVGL lock held, while an
     * SD operation must quiesce LVGL before using the shared TFT/SD SPI2 bus.
     * Reading the recorder snapshot here during STARTING therefore created a
     * lock inversion:
     *
     *   recorder START: recorder mutex -> waits for LVGL lock
     *   LVGL refresh:    LVGL lock      -> waits for recorder mutex
     *
     * The service snapshot already contains all information the UI needs in
     * STARTING. Once seismic_recorder_start() returns, the worker copies the
     * real recorder snapshot and changes the state to RECORDING.
     */
    if (snapshot->state != MEASUREMENT_SERVICE_STARTING) {
        seismic_recorder_snapshot_t recorder;
        if (seismic_recorder_get_snapshot(&recorder) == ESP_OK) {
            if (
                recorder.running ||
                snapshot->state == MEASUREMENT_SERVICE_RECORDING ||
                snapshot->state == MEASUREMENT_SERVICE_STOPPING
            ) {
                snprintf(
                    snapshot->session_id,
                    sizeof(snapshot->session_id),
                    "%s",
                    recorder.session_id
                );
                snapshot->samples_written = recorder.samples_written;
                snapshot->dropped_frames = recorder.dropped_frames;
                snapshot->recording_ms = recorder.recording_ms;
            }
        }
    }

    return ESP_OK;
}


bool measurement_service_snapshot_allows_object_mutation(
    const measurement_service_snapshot_t *snapshot
)
{
    if (snapshot == NULL || snapshot->busy) {
        return false;
    }

    return
        snapshot->state == MEASUREMENT_SERVICE_LIVE ||
        snapshot->state == MEASUREMENT_SERVICE_REVIEW ||
        snapshot->state == MEASUREMENT_SERVICE_ERROR;
}


bool measurement_service_snapshot_blocks_acquisition_stop(
    const measurement_service_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return true;
    }

    return snapshot->busy ||
        snapshot->state == MEASUREMENT_SERVICE_STARTING ||
        snapshot->state == MEASUREMENT_SERVICE_RECORDING ||
        snapshot->state == MEASUREMENT_SERVICE_STOPPING;
}


bool measurement_service_is_busy(void)
{
    if (!s_initialized || s_gate == NULL) {
        return false;
    }

    return uxSemaphoreGetCount(s_gate) == 0U;
}
