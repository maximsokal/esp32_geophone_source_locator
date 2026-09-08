#include "seismic_recorder.h"

#include <stdio.h>
#include <string.h>

#include "app_task_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "object_store.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_recorder_internal.h"
#include "storage_manager.h"


#define SEISMIC_RECORDER_EVENT_STOPPED          BIT0
#define SEISMIC_RECORDER_WRITE_PERIOD_MS        200U
#define SEISMIC_RECORDER_MIN_WRITE_FRAMES         8U
#define SEISMIC_RECORDER_BATCH_WAIT_MS            10U


static const char *TAG = "SEISMIC_RECORDER";

static SemaphoreHandle_t s_mutex = NULL;
static EventGroupHandle_t s_events = NULL;
static TaskHandle_t s_task = NULL;

static FILE *s_samples_file = NULL;
static object_store_profile_t s_profile;
static seismic_recorder_snapshot_t s_snapshot;
static seismic_frame_t s_batch[SEISMIC_RECORDER_BATCH_FRAMES];

static uint64_t s_cursor = 0U;
static uint64_t s_stop_target = 0U;
static int64_t s_started_us = 0;
static int64_t s_last_flush_us = 0;
static bool s_stop_requested = false;


static void seismic_recorder_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}


static void seismic_recorder_unlock(void)
{
    xSemaphoreGive(s_mutex);
}


static size_t seismic_recorder_target_batch_frames(
    uint32_t sample_rate_hz
)
{
    size_t target =
        (size_t)(((uint64_t)sample_rate_hz * SEISMIC_RECORDER_WRITE_PERIOD_MS) / 1000ULL);

    if (target < SEISMIC_RECORDER_MIN_WRITE_FRAMES) {
        target = SEISMIC_RECORDER_MIN_WRITE_FRAMES;
    }

    if (target > SEISMIC_RECORDER_BATCH_FRAMES) {
        target = SEISMIC_RECORDER_BATCH_FRAMES;
    }

    return target;
}


static uint64_t seismic_recorder_elapsed_ms_locked(void)
{
    if (s_started_us <= 0) {
        return s_snapshot.recording_ms;
    }

    const int64_t elapsed_us = esp_timer_get_time() - s_started_us;
    return elapsed_us > 0
        ? (uint64_t)(elapsed_us / 1000LL)
        : 0U;
}


static void seismic_recorder_copy_snapshot_locked(
    seismic_recorder_snapshot_t *snapshot
)
{
    *snapshot = s_snapshot;
    if (s_snapshot.running) {
        snapshot->recording_ms = seismic_recorder_elapsed_ms_locked();
    }
}


static void seismic_recorder_mark_error(
    esp_err_t error
)
{
    seismic_recorder_lock();
    if (s_snapshot.last_error == ESP_OK) {
        s_snapshot.last_error = error;
    }
    s_snapshot.state = SEISMIC_RECORDER_ERROR;
    s_stop_requested = true;
    seismic_recorder_unlock();
}


static void seismic_recorder_finalize(void)
{
    FILE *file = NULL;
    object_store_profile_t profile;
    seismic_recorder_snapshot_t final_snapshot;

    seismic_recorder_lock();
    file = s_samples_file;
    profile = s_profile;
    s_snapshot.state = SEISMIC_RECORDER_STOPPING;
    s_snapshot.recording_ms = seismic_recorder_elapsed_ms_locked();
    seismic_recorder_copy_snapshot_locked(&final_snapshot);
    seismic_recorder_unlock();

    esp_err_t close_err = ESP_OK;
    if (file != NULL) {
        close_err = seismic_recorder_fs_close(file);
    }

    seismic_recorder_lock();
    s_samples_file = NULL;
    s_snapshot.running = false;
    s_snapshot.recording_ms = seismic_recorder_elapsed_ms_locked();
    if (close_err != ESP_OK && s_snapshot.last_error == ESP_OK) {
        s_snapshot.last_error = close_err;
    }
    s_snapshot.state =
        s_snapshot.last_error == ESP_OK
            ? SEISMIC_RECORDER_IDLE
            : SEISMIC_RECORDER_ERROR;
    s_stop_requested = false;
    s_stop_target = s_cursor;
    seismic_recorder_copy_snapshot_locked(&final_snapshot);
    seismic_recorder_unlock();

    const char *status =
        final_snapshot.last_error == ESP_OK
            ? "complete"
            : "error";

    const esp_err_t metadata_err =
        seismic_recorder_json_write_metadata(
            &profile,
            &final_snapshot,
            status
        );

    if (metadata_err != ESP_OK) {
        seismic_recorder_lock();
        if (s_snapshot.last_error == ESP_OK) {
            s_snapshot.last_error = metadata_err;
        }
        s_snapshot.state = SEISMIC_RECORDER_ERROR;
        seismic_recorder_unlock();
    }

    ESP_LOGI(
        TAG,
        "Session %s stopped: samples=%llu drops=%llu err=%s",
        final_snapshot.session_id,
        (unsigned long long)final_snapshot.samples_written,
        (unsigned long long)final_snapshot.dropped_frames,
        esp_err_to_name(
            final_snapshot.last_error == ESP_OK
                ? metadata_err
                : final_snapshot.last_error
        )
    );

    xEventGroupSetBits(
        s_events,
        SEISMIC_RECORDER_EVENT_STOPPED
    );
}


static bool seismic_recorder_should_finalize_locked(void)
{
    return
        s_stop_requested &&
        s_cursor >= s_stop_target;
}


static void seismic_recorder_flush_if_due(
    FILE *file
)
{
    const int64_t now_us = esp_timer_get_time();

    seismic_recorder_lock();
    const bool due =
        s_snapshot.running &&
        file == s_samples_file &&
        now_us - s_last_flush_us >= SEISMIC_RECORDER_FLUSH_INTERVAL_US;
    if (due) {
        s_last_flush_us = now_us;
    }
    seismic_recorder_unlock();

    if (!due) {
        return;
    }

    const esp_err_t err = seismic_recorder_fs_flush(file, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Periodic SD flush failed: %s", esp_err_to_name(err));
        seismic_recorder_mark_error(err);
    }
}


static void seismic_recorder_task(void *argument)
{
    (void)argument;

    for (;;) {
        FILE *file = NULL;
        uint64_t cursor = 0U;
        uint64_t stop_target = 0U;
        bool running = false;
        bool stop_requested = false;

        seismic_recorder_lock();
        running = s_snapshot.running;
        file = s_samples_file;
        cursor = s_cursor;
        stop_requested = s_stop_requested;
        stop_target = s_stop_target;
        const bool finalize_now =
            running &&
            seismic_recorder_should_finalize_locked();
        seismic_recorder_unlock();

        if (finalize_now) {
            seismic_recorder_finalize();
            continue;
        }

        if (!running || file == NULL) {
            vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_IDLE_DELAY_MS));
            continue;
        }

        uint64_t copy_cursor = cursor;
        uint64_t dropped = 0U;
        size_t frame_count = 0U;
        seismic_buffer_info_t info;

        const esp_err_t read_err = seismic_buffer_copy_since(
            &copy_cursor,
            s_batch,
            SEISMIC_RECORDER_BATCH_FRAMES,
            &frame_count,
            &dropped,
            &info
        );

        if (read_err == ESP_ERR_TIMEOUT) {
            taskYIELD();
            continue;
        }

        if (read_err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Recorder lost acquisition generation: %s",
                esp_err_to_name(read_err)
            );
            seismic_recorder_mark_error(read_err);
            seismic_recorder_lock();
            s_stop_target = s_cursor;
            seismic_recorder_unlock();
            continue;
        }

        /*
         * Do not open a TFT/SD arbitration window for every 500 Hz sample.
         * Wait until roughly 200 ms of frames are ready, then write one
         * bounded batch. STOP bypasses this threshold so the exact stop cursor
         * can still be drained immediately.
         */
        if (!stop_requested && dropped == 0U && frame_count > 0U) {
            const size_t target_batch =
                seismic_recorder_target_batch_frames(info.sample_rate_hz);

            if (frame_count < target_batch) {
                vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_BATCH_WAIT_MS));
                continue;
            }
        }

        size_t allowed_count = frame_count;
        uint64_t effective_dropped = dropped;
        uint64_t first_frame =
            frame_count > 0U
                ? copy_cursor - (uint64_t)frame_count + 1U
                : cursor + dropped + 1U;

        if (stop_requested) {
            const uint64_t remaining_to_target =
                stop_target > cursor
                    ? stop_target - cursor
                    : 0U;

            if (effective_dropped > remaining_to_target) {
                effective_dropped = remaining_to_target;
            }

            if (frame_count > 0U) {
                if (first_frame > stop_target) {
                    allowed_count = 0U;
                }
                else {
                    const uint64_t permitted =
                        stop_target - first_frame + 1U;
                    if ((uint64_t)allowed_count > permitted) {
                        allowed_count = (size_t)permitted;
                    }
                }
            }
        }

        size_t frames_written = 0U;
        esp_err_t write_err = ESP_OK;
        if (allowed_count > 0U) {
            write_err = seismic_recorder_fs_write_frames(
                file,
                s_batch,
                allowed_count,
                &frames_written
            );
        }

        seismic_recorder_lock();
        if (s_snapshot.running && file == s_samples_file) {
            s_snapshot.dropped_frames += effective_dropped;
            s_snapshot.samples_written += frames_written;

            if (frames_written > 0U) {
                s_cursor =
                    first_frame +
                    (uint64_t)frames_written -
                    1U;
            }
            else if (effective_dropped > 0U) {
                s_cursor += effective_dropped;
            }

            if (write_err != ESP_OK) {
                if (s_snapshot.last_error == ESP_OK) {
                    s_snapshot.last_error = write_err;
                }
                s_snapshot.state = SEISMIC_RECORDER_ERROR;
                s_stop_requested = true;
                s_stop_target = s_cursor;
            }
        }
        const bool done =
            s_snapshot.running &&
            seismic_recorder_should_finalize_locked();
        seismic_recorder_unlock();

        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "SD sample write failed: %s", esp_err_to_name(write_err));
        }

        if (done) {
            seismic_recorder_finalize();
            continue;
        }

        if (frames_written > 0U) {
            seismic_recorder_flush_if_due(file);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_IDLE_DELAY_MS));
    }
}


esp_err_t seismic_recorder_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    if (!storage_manager_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_profile, 0, sizeof(s_profile));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.initialized = true;
    s_snapshot.state = SEISMIC_RECORDER_IDLE;
    s_snapshot.last_error = ESP_OK;

    xEventGroupSetBits(s_events, SEISMIC_RECORDER_EVENT_STOPPED);

    BaseType_t created = xTaskCreatePinnedToCore(
        seismic_recorder_task,
        "seismic_recorder",
        APP_TASK_STACK_SEISMIC_RECORDER,
        NULL,
        APP_TASK_PRIORITY_SEISMIC_RECORDER,
        &s_task,
        APP_TASK_CORE_SERVICES
    );

    if (created != pdPASS) {
        vEventGroupDelete(s_events);
        vSemaphoreDelete(s_mutex);
        s_events = NULL;
        s_mutex = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Recorder ready on core %d; batch=%u frames, write window=%u ms",
        APP_TASK_CORE_SERVICES,
        (unsigned)SEISMIC_RECORDER_BATCH_FRAMES,
        (unsigned)SEISMIC_RECORDER_WRITE_PERIOD_MS
    );

    return ESP_OK;
}


esp_err_t seismic_recorder_start(
    const char *object_id
)
{
    if (object_id == NULL || object_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL || s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    seismic_acquisition_snapshot_t acquisition;
    esp_err_t err = seismic_acquisition_get_snapshot(&acquisition);
    if (err != ESP_OK || !acquisition.running || acquisition.sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    object_store_profile_t profile;
    err = object_store_get_profile(object_id, &profile);
    if (err != ESP_OK) {
        return err;
    }

    seismic_frame_t scratch;
    size_t scratch_count = 0U;
    seismic_buffer_info_t buffer_info;
    err = seismic_buffer_copy_latest(
        &scratch,
        1U,
        &scratch_count,
        &buffer_info
    );
    if (err != ESP_OK) {
        return err;
    }

    seismic_recorder_lock();
    if (s_snapshot.running || s_samples_file != NULL) {
        seismic_recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE] = "";
    FILE *file = NULL;
    err = seismic_recorder_fs_create_session(
        object_id,
        acquisition.sample_rate_hz,
        SEISMIC_CHANNEL_COUNT,
        session_id,
        &file
    );
    if (err != ESP_OK) {
        s_snapshot.last_error = err;
        s_snapshot.state = SEISMIC_RECORDER_ERROR;
        seismic_recorder_unlock();
        return err;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.initialized = true;
    s_snapshot.running = true;
    s_snapshot.state = SEISMIC_RECORDER_RECORDING;
    s_snapshot.sample_rate_hz = acquisition.sample_rate_hz;
    s_snapshot.channel_count = SEISMIC_CHANNEL_COUNT;
    s_snapshot.last_error = ESP_OK;

    snprintf(
        s_snapshot.object_id,
        sizeof(s_snapshot.object_id),
        "%s",
        object_id
    );
    snprintf(
        s_snapshot.session_id,
        sizeof(s_snapshot.session_id),
        "%s",
        session_id
    );
    snprintf(
        s_snapshot.source_name,
        sizeof(s_snapshot.source_name),
        "%s",
        acquisition.source_name != NULL
            ? acquisition.source_name
            : "UNKNOWN"
    );

    s_profile = profile;
    s_samples_file = file;
    s_cursor = buffer_info.total_frames;
    s_stop_target = s_cursor;
    s_stop_requested = false;
    s_started_us = esp_timer_get_time();
    s_last_flush_us = s_started_us;

    xEventGroupClearBits(s_events, SEISMIC_RECORDER_EVENT_STOPPED);

    seismic_recorder_snapshot_t initial_snapshot;
    seismic_recorder_copy_snapshot_locked(&initial_snapshot);

    err = seismic_recorder_json_write_metadata(
        &s_profile,
        &initial_snapshot,
        "recording"
    );

    if (err != ESP_OK) {
        s_snapshot.last_error = err;
        s_snapshot.state = SEISMIC_RECORDER_ERROR;
        s_stop_requested = true;
        s_stop_target = s_cursor;
    }

    seismic_recorder_unlock();

    if (err != ESP_OK) {
        (void)seismic_recorder_stop();
        return err;
    }

    ESP_LOGI(
        TAG,
        "Recording %s/%s at %lu Hz from cursor %llu (target batch=%u)",
        object_id,
        session_id,
        (unsigned long)acquisition.sample_rate_hz,
        (unsigned long long)buffer_info.total_frames,
        (unsigned)seismic_recorder_target_batch_frames(acquisition.sample_rate_hz)
    );

    return ESP_OK;
}


esp_err_t seismic_recorder_stop(void)
{
    if (s_mutex == NULL || s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    seismic_recorder_lock();
    if (!s_snapshot.running) {
        seismic_recorder_unlock();
        return ESP_OK;
    }
    seismic_recorder_unlock();

    seismic_frame_t scratch;
    size_t scratch_count = 0U;
    seismic_buffer_info_t info;
    const esp_err_t info_err = seismic_buffer_copy_latest(
        &scratch,
        1U,
        &scratch_count,
        &info
    );

    seismic_recorder_lock();
    if (!s_snapshot.running) {
        seismic_recorder_unlock();
        return ESP_OK;
    }

    s_stop_requested = true;
    s_snapshot.state = SEISMIC_RECORDER_STOPPING;
    s_stop_target =
        info_err == ESP_OK && info.total_frames >= s_cursor
            ? info.total_frames
            : s_cursor;
    seismic_recorder_unlock();

    const EventBits_t bits = xEventGroupWaitBits(
        s_events,
        SEISMIC_RECORDER_EVENT_STOPPED,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(SEISMIC_RECORDER_STOP_TIMEOUT_MS)
    );

    if ((bits & SEISMIC_RECORDER_EVENT_STOPPED) == 0U) {
        ESP_LOGE(TAG, "Timed out stopping recorder");
        return ESP_ERR_TIMEOUT;
    }

    seismic_recorder_snapshot_t snapshot;
    (void)seismic_recorder_get_snapshot(&snapshot);
    return snapshot.last_error;
}


esp_err_t seismic_recorder_get_snapshot(
    seismic_recorder_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        return ESP_ERR_INVALID_STATE;
    }

    seismic_recorder_lock();
    seismic_recorder_copy_snapshot_locked(snapshot);
    seismic_recorder_unlock();
    return ESP_OK;
}


bool seismic_recorder_is_recording_object(
    const char *object_id
)
{
    if (object_id == NULL || s_mutex == NULL) {
        return false;
    }

    seismic_recorder_lock();
    const bool matches =
        s_snapshot.running &&
        strcmp(s_snapshot.object_id, object_id) == 0;
    seismic_recorder_unlock();
    return matches;
}


esp_err_t seismic_recorder_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return seismic_recorder_fs_list_sessions(
        object_id,
        sessions,
        max_sessions,
        session_count
    );
}
