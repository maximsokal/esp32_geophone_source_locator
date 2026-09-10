#include "seismic_recorder.h"

#include "runtime_observability.h"

#include <stdio.h>
#include <stdlib.h>
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
#include "seismic_recorder_start_policy.h"
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
static int64_t s_last_batch_log_us = 0;
static bool s_stop_requested = false;
static bool s_starting = false;


static void seismic_recorder_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}


static void seismic_recorder_unlock(void)
{
    xSemaphoreGive(s_mutex);
}


const char *seismic_recorder_stop_stage_name(
    seismic_recorder_stop_stage_t stage
)
{
    switch (stage) {
        case SEISMIC_RECORDER_STOP_STAGE_DRAIN:
            return "drain";
        case SEISMIC_RECORDER_STOP_STAGE_FLUSH:
            return "flush";
        case SEISMIC_RECORDER_STOP_STAGE_SYNC:
            return "sync";
        case SEISMIC_RECORDER_STOP_STAGE_CLOSE:
            return "close";
        case SEISMIC_RECORDER_STOP_STAGE_METADATA:
            return "metadata";
        case SEISMIC_RECORDER_STOP_STAGE_COMPLETE:
            return "complete";
        case SEISMIC_RECORDER_STOP_STAGE_ERROR:
            return "error";
        case SEISMIC_RECORDER_STOP_STAGE_NONE:
        default:
            return "none";
    }
}


static void seismic_recorder_set_stop_stage(
    seismic_recorder_stop_stage_t stage
)
{
    seismic_recorder_lock();
    s_snapshot.stop_stage = stage;
    seismic_recorder_unlock();

    ESP_LOGI(
        TAG,
        "STOP_STAGE stage=%s",
        seismic_recorder_stop_stage_name(stage)
    );
}


static void seismic_recorder_record_stop_error(
    seismic_recorder_stop_stage_t failed_stage,
    esp_err_t error,
    seismic_recorder_stop_stage_t next_stage
)
{
    if (error == ESP_OK) {
        return;
    }

    seismic_recorder_lock();
    if (s_snapshot.last_error == ESP_OK) {
        s_snapshot.last_error = error;
    }
    if (s_snapshot.stop_stage_error == ESP_OK) {
        s_snapshot.stop_stage_error = error;
        s_snapshot.stop_error_stage = failed_stage;
    }
    s_snapshot.state = SEISMIC_RECORDER_STOPPING;
    s_stop_requested = true;
    s_stop_target = s_cursor;
    s_snapshot.stop_stage = next_stage;
    seismic_recorder_unlock();

    ESP_LOGE(
        TAG,
        "STOP_STAGE failed=%s err=%s next=%s",
        seismic_recorder_stop_stage_name(failed_stage),
        esp_err_to_name(error),
        seismic_recorder_stop_stage_name(next_stage)
    );
}


/*
 * Publish recorder liveness only after the current service operation has
 * returned. In particular, this must follow buffer copies, SD writes and
 * flushes so a blocked storage path cannot look healthy merely because the
 * task entered its loop.
 */
static void seismic_recorder_observe(void)
{
    uint64_t samples_written;

    seismic_recorder_lock();
    samples_written = s_snapshot.samples_written;
    seismic_recorder_unlock();

    runtime_observability_beat(
        RUNTIME_TASK_RECORDER,
        samples_written
    );
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


static void seismic_recorder_set_start_error(
    esp_err_t error
)
{
    seismic_recorder_lock();
    s_starting = false;
    if (!s_snapshot.running && s_samples_file == NULL) {
        s_snapshot.last_error = error;
        s_snapshot.state = SEISMIC_RECORDER_ERROR;
    }
    seismic_recorder_unlock();
}


static void seismic_recorder_discard_uncommitted_session(
    const char *object_id,
    const char *session_id,
    FILE *file
)
{
    if (file != NULL) {
        const esp_err_t close_err = seismic_recorder_fs_close(file);
        if (close_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to close uncommitted session file: %s",
                esp_err_to_name(close_err)
            );
        }
    }

    if (
        object_id != NULL &&
        session_id != NULL &&
        session_id[0] != '\0'
    ) {
        const esp_err_t delete_err =
            seismic_recorder_fs_delete_session(
                object_id,
                session_id
            );

        if (
            delete_err != ESP_OK &&
            delete_err != ESP_ERR_NOT_FOUND
        ) {
            ESP_LOGW(
                TAG,
                "Failed to remove uncommitted session %s/%s: %s",
                object_id,
                session_id,
                esp_err_to_name(delete_err)
            );
        }
    }
}


static void seismic_recorder_stop_step(void)
{
    FILE *file = NULL;
    seismic_recorder_stop_stage_t stage;

    seismic_recorder_lock();
    file = s_samples_file;
    stage = s_snapshot.stop_stage;
    seismic_recorder_unlock();

    switch (stage) {
        case SEISMIC_RECORDER_STOP_STAGE_FLUSH: {
            const esp_err_t err =
                file == NULL
                    ? ESP_ERR_INVALID_STATE
                    : seismic_recorder_fs_flush(file, false);
            if (err != ESP_OK) {
                seismic_recorder_record_stop_error(
                    SEISMIC_RECORDER_STOP_STAGE_FLUSH,
                    err,
                    SEISMIC_RECORDER_STOP_STAGE_SYNC
                );
            }
            else {
                seismic_recorder_set_stop_stage(
                    SEISMIC_RECORDER_STOP_STAGE_SYNC
                );
            }
            break;
        }

        case SEISMIC_RECORDER_STOP_STAGE_SYNC: {
            const esp_err_t err =
                file == NULL
                    ? ESP_ERR_INVALID_STATE
                    : seismic_recorder_fs_sync(file);
            if (err != ESP_OK) {
                seismic_recorder_record_stop_error(
                    SEISMIC_RECORDER_STOP_STAGE_SYNC,
                    err,
                    SEISMIC_RECORDER_STOP_STAGE_CLOSE
                );
            }
            else {
                seismic_recorder_set_stop_stage(
                    SEISMIC_RECORDER_STOP_STAGE_CLOSE
                );
            }
            break;
        }

        case SEISMIC_RECORDER_STOP_STAGE_CLOSE: {
            const esp_err_t err =
                file == NULL
                    ? ESP_ERR_INVALID_STATE
                    : seismic_recorder_fs_close(file);

            seismic_recorder_lock();
            if (file == s_samples_file) {
                s_samples_file = NULL;
            }
            seismic_recorder_unlock();

            if (err != ESP_OK) {
                seismic_recorder_record_stop_error(
                    SEISMIC_RECORDER_STOP_STAGE_CLOSE,
                    err,
                    SEISMIC_RECORDER_STOP_STAGE_METADATA
                );
            }
            else {
                seismic_recorder_set_stop_stage(
                    SEISMIC_RECORDER_STOP_STAGE_METADATA
                );
            }
            break;
        }

        case SEISMIC_RECORDER_STOP_STAGE_METADATA: {
            seismic_recorder_snapshot_t final_snapshot;

            seismic_recorder_lock();
            s_snapshot.recording_ms = seismic_recorder_elapsed_ms_locked();
            seismic_recorder_copy_snapshot_locked(&final_snapshot);
            final_snapshot.running = false;
            final_snapshot.state =
                final_snapshot.last_error == ESP_OK
                    ? SEISMIC_RECORDER_IDLE
                    : SEISMIC_RECORDER_ERROR;
            final_snapshot.stop_stage =
                SEISMIC_RECORDER_STOP_STAGE_METADATA;
            seismic_recorder_unlock();

            const char *status =
                final_snapshot.last_error == ESP_OK
                    ? "complete"
                    : "error";
            const esp_err_t metadata_err =
                seismic_recorder_json_write_metadata(
                    &s_profile,
                    &final_snapshot,
                    status
                );
            if (metadata_err != ESP_OK) {
                seismic_recorder_record_stop_error(
                    SEISMIC_RECORDER_STOP_STAGE_METADATA,
                    metadata_err,
                    SEISMIC_RECORDER_STOP_STAGE_ERROR
                );
            }

            seismic_recorder_lock();
            const bool complete = s_snapshot.last_error == ESP_OK;
            s_snapshot.running = false;
            s_snapshot.state =
                complete
                    ? SEISMIC_RECORDER_IDLE
                    : SEISMIC_RECORDER_ERROR;
            s_stop_requested = false;
            s_stop_target = s_cursor;
            s_snapshot.stop_stage =
                complete
                    ? SEISMIC_RECORDER_STOP_STAGE_COMPLETE
                    : SEISMIC_RECORDER_STOP_STAGE_ERROR;
            seismic_recorder_copy_snapshot_locked(&final_snapshot);
            seismic_recorder_unlock();

            ESP_LOGI(
                TAG,
                "STOP_COMPLETE stage=%s error_stage=%s stage_error=%s session=%s samples=%llu drops=%llu err=%s",
                seismic_recorder_stop_stage_name(final_snapshot.stop_stage),
                seismic_recorder_stop_stage_name(final_snapshot.stop_error_stage),
                esp_err_to_name(final_snapshot.stop_stage_error),
                final_snapshot.session_id,
                (unsigned long long)final_snapshot.samples_written,
                (unsigned long long)final_snapshot.dropped_frames,
                esp_err_to_name(final_snapshot.last_error)
            );

            xEventGroupSetBits(
                s_events,
                SEISMIC_RECORDER_EVENT_STOPPED
            );
            break;
        }

        case SEISMIC_RECORDER_STOP_STAGE_NONE:
        case SEISMIC_RECORDER_STOP_STAGE_DRAIN:
        case SEISMIC_RECORDER_STOP_STAGE_COMPLETE:
        case SEISMIC_RECORDER_STOP_STAGE_ERROR:
        default:
            seismic_recorder_record_stop_error(
                stage,
                ESP_ERR_INVALID_STATE,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            break;
    }
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
        seismic_recorder_record_stop_error(
            SEISMIC_RECORDER_STOP_STAGE_FLUSH,
            err,
            SEISMIC_RECORDER_STOP_STAGE_SYNC
        );
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
        seismic_recorder_stop_stage_t stop_stage;

        seismic_recorder_lock();
        running = s_snapshot.running;
        file = s_samples_file;
        cursor = s_cursor;
        stop_requested = s_stop_requested;
        stop_target = s_stop_target;
        stop_stage = s_snapshot.stop_stage;
        seismic_recorder_unlock();

        if (!running) {
            vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_IDLE_DELAY_MS));
            seismic_recorder_observe();
            continue;
        }

        /*
         * STOP is a command to this worker, not a synchronous call-chain.
         * Once drain is complete, execute exactly one I/O stage per worker
         * turn. The service task can continue polling and every stage result
         * is published through the recorder snapshot.
         */
        if (stop_requested && stop_stage != SEISMIC_RECORDER_STOP_STAGE_DRAIN) {
            seismic_recorder_stop_step();
            seismic_recorder_observe();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (stop_requested && cursor >= stop_target) {
            seismic_recorder_set_stop_stage(
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            seismic_recorder_observe();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (file == NULL) {
            seismic_recorder_record_stop_error(
                SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                ESP_ERR_INVALID_STATE,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            seismic_recorder_observe();
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
            seismic_recorder_observe();
            continue;
        }

        if (read_err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Recorder lost acquisition generation: %s",
                esp_err_to_name(read_err)
            );
            seismic_recorder_record_stop_error(
                SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                read_err,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            seismic_recorder_observe();
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
                seismic_recorder_observe();
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

        uint64_t cursor_after = cursor;
        uint64_t total_dropped = 0U;
        bool batch_log_due = false;

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

            cursor_after = s_cursor;
            total_dropped = s_snapshot.dropped_frames;

            const int64_t now_us = esp_timer_get_time();
            if (
                (frames_written > 0U || effective_dropped > 0U) &&
                (
                    s_last_batch_log_us == 0 ||
                    now_us - s_last_batch_log_us >= 1000000LL
                )
            ) {
                s_last_batch_log_us = now_us;
                batch_log_due = true;
            }
        }
        seismic_recorder_unlock();

        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "SD sample write failed: %s", esp_err_to_name(write_err));
            seismic_recorder_record_stop_error(
                SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                write_err,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
        }

        if (batch_log_due) {
            const uint64_t backlog =
                info.total_frames > cursor_after
                    ? info.total_frames - cursor_after
                    : 0U;
            ESP_LOGI(
                TAG,
                "RECORDER_BATCH requested=%u written=%u cursor=%llu producer_total=%llu backlog=%llu dropped=%llu total_dropped=%llu stop=%u",
                (unsigned)allowed_count,
                (unsigned)frames_written,
                (unsigned long long)cursor_after,
                (unsigned long long)info.total_frames,
                (unsigned long long)backlog,
                (unsigned long long)dropped,
                (unsigned long long)total_dropped,
                stop_requested ? 1U : 0U
            );
        }

        if (write_err == ESP_OK && frames_written > 0U) {
            seismic_recorder_flush_if_due(file);
            seismic_recorder_observe();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_IDLE_DELAY_MS));
        seismic_recorder_observe();
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
    s_starting = false;
    s_last_batch_log_us = 0;

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
        ESP_LOGE(
            TAG,
            "START rejected: recorder is not initialized mutex=%s events=%s",
            s_mutex != NULL ? "ready" : "missing",
            s_events != NULL ? "ready" : "missing"
        );
        return ESP_ERR_INVALID_STATE;
    }

    seismic_acquisition_snapshot_t acquisition;
    esp_err_t err = seismic_acquisition_get_snapshot(&acquisition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "START rejected: acquisition snapshot failed err=%s", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    err = seismic_recorder_start_validate_acquisition(&acquisition);
    if (err != ESP_OK) {
        if (!acquisition.running) {
            ESP_LOGE(
                TAG,
                "START rejected: acquisition task is not running source=%s presence=%s serial=%s",
                acquisition.source_name != NULL ? acquisition.source_name : "UNKNOWN",
                acquisition.source_healthy ? "healthy" : "stale",
                acquisition.serial_suspicious ? "suspicious" : "not-suspicious"
            );
        }
        else if (acquisition.sample_rate_hz == 0U) {
            ESP_LOGE(
                TAG,
                "START rejected: acquisition task has zero sample rate source=%s",
                acquisition.source_name != NULL ? acquisition.source_name : "UNKNOWN"
            );
        }
        else if (
            !acquisition.latest_valid ||
            acquisition.samples == 0U
        ) {
            ESP_LOGE(
                TAG,
                "START rejected: no successful acquisition sample yet source=%s latest_valid=%s samples=%llu",
                acquisition.source_name != NULL ? acquisition.source_name : "UNKNOWN",
                acquisition.latest_valid ? "yes" : "no",
                (unsigned long long)acquisition.samples
            );
        }
        else {
            ESP_LOGE(
                TAG,
                "START rejected: acquisition sample is not fresh source=%s samples=%llu last_sample_us=%lld",
                acquisition.source_name != NULL ? acquisition.source_name : "UNKNOWN",
                (unsigned long long)acquisition.samples,
                (long long)acquisition.last_sample_timestamp_us
            );
        }
        return err;
    }
    if (!acquisition.source_healthy || acquisition.serial_suspicious) {
        ESP_LOGW(
            TAG,
            "START continuing with acquisition task running: presence=%s serial=%s pattern=%s",
            acquisition.source_healthy ? "healthy" : "stale",
            acquisition.serial_suspicious ? "suspicious" : "not-suspicious",
            acquisition.serial_pattern != NULL ? acquisition.serial_pattern : "UNKNOWN"
        );
    }

    object_store_profile_t *profile = malloc(sizeof(*profile));
    if (profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = object_store_get_profile(object_id, profile);
    if (err != ESP_OK) {
        free(profile);
        return err;
    }

    /*
     * Reserve the START operation with the recorder mutex, then release it
     * before any SD access. SD operations quiesce LVGL because TFT and SD share
     * SPI2. Holding the recorder mutex while waiting for that LVGL lock caused
     * the measurement screen to deadlock when its refresh path tried to read
     * the recorder snapshot while already holding LVGL.
     */
    seismic_recorder_lock();
    if (s_starting || s_snapshot.running || s_samples_file != NULL) {
        ESP_LOGE(
            TAG,
            "START rejected: recorder busy starting=%s running=%s file=%s",
            s_starting ? "yes" : "no",
            s_snapshot.running ? "yes" : "no",
            s_samples_file != NULL ? "open" : "closed"
        );
        seismic_recorder_unlock();
        free(profile);
        return ESP_ERR_INVALID_STATE;
    }
    s_starting = true;
    seismic_recorder_unlock();

    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE] = "";
    FILE *file = NULL;

    ESP_LOGI(
        TAG,
        "Preparing session for object=%s",
        object_id
    );

    err = seismic_recorder_fs_create_session(
        object_id,
        acquisition.sample_rate_hz,
        SEISMIC_CHANNEL_COUNT,
        session_id,
        &file
    );
    if (err != ESP_OK) {
        free(profile);
        seismic_recorder_set_start_error(err);
        return err;
    }

    seismic_recorder_snapshot_t initial_snapshot;
    memset(&initial_snapshot, 0, sizeof(initial_snapshot));
    initial_snapshot.initialized = true;
    initial_snapshot.running = true;
    initial_snapshot.state = SEISMIC_RECORDER_RECORDING;
    initial_snapshot.stop_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    initial_snapshot.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    initial_snapshot.sample_rate_hz = acquisition.sample_rate_hz;
    initial_snapshot.channel_count = SEISMIC_CHANNEL_COUNT;
    initial_snapshot.last_error = ESP_OK;
    initial_snapshot.stop_stage_error = ESP_OK;

    snprintf(
        initial_snapshot.object_id,
        sizeof(initial_snapshot.object_id),
        "%s",
        object_id
    );
    snprintf(
        initial_snapshot.session_id,
        sizeof(initial_snapshot.session_id),
        "%s",
        session_id
    );
    snprintf(
        initial_snapshot.source_name,
        sizeof(initial_snapshot.source_name),
        "%s",
        acquisition.source_name != NULL
            ? acquisition.source_name
            : "UNKNOWN"
    );

    /*
     * Write the initial metadata before publishing the recorder as RUNNING.
     * No recorder mutex is held across this SD transaction.
     */
    err = seismic_recorder_json_write_metadata(
        profile,
        &initial_snapshot,
        "recording"
    );
    if (err != ESP_OK) {
        free(profile);
        seismic_recorder_discard_uncommitted_session(
            object_id,
            session_id,
            file
        );
        seismic_recorder_set_start_error(err);
        return err;
    }

    /*
     * Session creation and metadata can take hundreds of milliseconds on FATFS.
     * Start the data cursor at the newest frame only after that setup completes,
     * so the recording does not include stale pre-START samples.
     */
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
        free(profile);
        seismic_recorder_discard_uncommitted_session(
            object_id,
            session_id,
            file
        );
        seismic_recorder_set_start_error(err);
        return err;
    }

    const int64_t started_us = esp_timer_get_time();

    seismic_recorder_lock();
    if (s_snapshot.running || s_samples_file != NULL) {
        s_starting = false;
        seismic_recorder_unlock();

        free(profile);
        seismic_recorder_discard_uncommitted_session(
            object_id,
            session_id,
            file
        );
        return ESP_ERR_INVALID_STATE;
    }

    s_snapshot = initial_snapshot;
    s_profile = *profile;
    s_samples_file = file;
    s_cursor = buffer_info.total_frames;
    s_stop_target = s_cursor;
    s_stop_requested = false;
    s_last_batch_log_us = started_us;
    s_started_us = started_us;
    s_last_flush_us = started_us;
    s_starting = false;

    xEventGroupClearBits(s_events, SEISMIC_RECORDER_EVENT_STOPPED);
    seismic_recorder_unlock();
    free(profile);

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
    if (s_stop_requested) {
        seismic_recorder_unlock();
        return ESP_OK;
    }
    seismic_recorder_unlock();

    seismic_frame_t scratch;
    size_t scratch_count = 0U;
    seismic_buffer_info_t info = {0};
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
    s_snapshot.stop_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    s_snapshot.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_snapshot.stop_stage_error = ESP_OK;
    s_stop_target =
        info_err == ESP_OK && info.total_frames >= s_cursor
            ? info.total_frames
            : s_cursor;
    const uint64_t target = s_stop_target;
    const uint64_t cursor = s_cursor;
    seismic_recorder_unlock();

    const uint64_t backlog =
        info.total_frames > cursor
            ? info.total_frames - cursor
            : 0U;
    ESP_LOGI(
        TAG,
        "STOP_REQUEST stage=drain cursor=%llu target=%llu producer_total=%llu buffered=%u backlog=%llu info_err=%s",
        (unsigned long long)cursor,
        (unsigned long long)target,
        (unsigned long long)info.total_frames,
        (unsigned)info.buffered_frames,
        (unsigned long long)backlog,
        esp_err_to_name(info_err)
    );

    return ESP_OK;
}


esp_err_t seismic_recorder_wait_for_stop(void)
{
    if (s_mutex == NULL || s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    seismic_recorder_lock();
    const bool running = s_snapshot.running;
    const bool stopping =
        s_stop_requested ||
        s_snapshot.state == SEISMIC_RECORDER_STOPPING;
    const esp_err_t current_error = s_snapshot.last_error;
    seismic_recorder_unlock();

    if (!running) {
        return current_error;
    }

    if (!stopping) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xEventGroupWaitBits(
        s_events,
        SEISMIC_RECORDER_EVENT_STOPPED,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    seismic_recorder_snapshot_t snapshot;
    const esp_err_t err = seismic_recorder_get_snapshot(&snapshot);
    return err == ESP_OK ? snapshot.last_error : err;
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
