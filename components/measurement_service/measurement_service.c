#include "measurement_service.h"

#include <stdio.h>
#include <string.h>

#include "app_task_config.h"

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

} measurement_command_t;


static const char *TAG = "MEASUREMENT_SERVICE";

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_gate = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static measurement_service_snapshot_t s_snapshot;
static bool s_initialized = false;


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


static void measurement_service_release_gate(void)
{
    if (s_gate != NULL) {
        (void)xSemaphoreGive(s_gate);
    }
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


static void measurement_service_process_start(void)
{
    measurement_service_lock();
    s_snapshot.state = MEASUREMENT_SERVICE_STARTING;
    s_snapshot.last_error = ESP_OK;
    s_snapshot.stop_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_snapshot.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_snapshot.stop_stage_error = ESP_OK;
    s_snapshot.session_id[0] = '\0';
    s_snapshot.samples_written = 0U;
    s_snapshot.dropped_frames = 0U;
    s_snapshot.recording_ms = 0U;
    measurement_service_unlock();

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

    runtime_observability_log_snapshot("recording-active");
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

    measurement_service_log_stack_hwm("after-stop");
    runtime_observability_log_snapshot("after-recorder-finalization");
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
    s_snapshot.session_id[0] = '\0';
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
    s_snapshot.session_id[0] = '\0';
    s_snapshot.samples_written = 0U;
    s_snapshot.dropped_frames = 0U;
    s_snapshot.recording_ms = 0U;
    measurement_service_unlock();

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

        if (command == MEASUREMENT_COMMAND_SHUTDOWN) {
            break;
        }

        switch (command) {
            case MEASUREMENT_COMMAND_START:
                measurement_service_process_start();
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
    measurement_command_t command
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
    measurement_command_t command
)
{
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
        measurement_service_command_allowed_locked(command);
    measurement_service_unlock();

    if (!allowed) {
        measurement_service_release_gate();
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        measurement_service_release_gate();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
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

    measurement_command_t command = MEASUREMENT_COMMAND_SHUTDOWN;

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
    s_initialized = false;

    ESP_LOGI(TAG, "Shutdown complete");
}


esp_err_t measurement_service_start_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_START);
}


esp_err_t measurement_service_stop_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_STOP);
}


esp_err_t measurement_service_keep_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_KEEP);
}


esp_err_t measurement_service_delete_async(void)
{
    return measurement_service_submit(MEASUREMENT_COMMAND_DELETE);
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

    measurement_service_lock();
    *snapshot = s_snapshot;
    measurement_service_unlock();

    snapshot->busy = measurement_service_is_busy();

    object_service_summary_t object;
    const esp_err_t object_err = object_service_get_active(&object);
    if (object_err == ESP_OK && object.active && object.id[0] != '\0') {
        snapshot->active_object_present = true;
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
    else {
        snapshot->active_object_present = false;
        if (
            snapshot->state == MEASUREMENT_SERVICE_LIVE ||
            snapshot->state == MEASUREMENT_SERVICE_ERROR
        ) {
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


bool measurement_service_is_busy(void)
{
    if (!s_initialized || s_gate == NULL) {
        return false;
    }

    return uxSemaphoreGetCount(s_gate) == 0U;
}
