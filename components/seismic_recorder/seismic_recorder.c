#include "seismic_recorder.h"

#include "runtime_observability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "object_store.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_recorder_cache.h"
#include "seismic_recorder_cadence.h"
#include "seismic_recorder_internal.h"
#include "seismic_recorder_start_policy.h"
#include "storage_manager.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB
#define CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB 1024
#endif

#define SEISMIC_RECORDER_EVENT_STOPPED          BIT0
#define SEISMIC_RECORDER_WRITE_PERIOD_MS        500U
#define SEISMIC_RECORDER_MIN_WRITE_FRAMES         8U
#define SEISMIC_RECORDER_BATCH_WAIT_MS            10U


static const char *TAG = "SEISMIC_RECORDER";

static SemaphoreHandle_t s_mutex = NULL;
static EventGroupHandle_t s_events = NULL;
static TaskHandle_t s_task = NULL;

static FILE *s_samples_file = NULL;
static object_store_profile_t s_profile;
static seismic_recorder_snapshot_t s_snapshot;
static seismic_recorder_cadence_t s_cadence_start;

/*
 * The worker copy buffer is PSRAM-backed and is reused for both acquisition
 * ingestion and one bounded write-behind flush. The actual SDSPI writer still
 * copies each <=256-frame flush through its internal DMA_ATTR staging buffer.
 */
static seismic_frame_t *s_batch = NULL;

/*
 * Write-behind cache: acquisition is acknowledged into this PSRAM ring first.
 * s_ingest_cursor is the source-frame frontier safely copied out of the small
 * realtime seismic_buffer. s_cursor remains the source-frame frontier already
 * handed to samples.bin. Keeping the two frontiers separate prevents a slow
 * SD operation from forcing the 4-second realtime ring to be the only backlog.
 */
static seismic_recorder_cache_t s_cache;
static seismic_recorder_cache_slot_t *s_cache_storage = NULL;
static size_t s_cache_capacity = 0U;
static uint64_t s_ingest_cursor = 0U;
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


static seismic_recorder_cadence_t seismic_recorder_cadence_snapshot(
    const seismic_acquisition_snapshot_t *acquisition
)
{
    seismic_recorder_cadence_t cadence = {0};
    if (acquisition == NULL || !acquisition->cadence_available) {
        return cadence;
    }

    cadence.available = true;
    cadence.timer_ticks = acquisition->timer_ticks;
    cadence.conversion_attempts = acquisition->conversion_attempts;
    cadence.collapsed_periods = acquisition->collapsed_periods;
    return cadence;
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
    /* Error shutdown abandons any still-dirty PSRAM tail. Metadata keeps the
     * exact persisted sample count, never pretending RAM-only frames survived. */
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


/* Publish liveness only after the current bounded operation has returned. */
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
                "STOP_COMPLETE stage=%s error_stage=%s stage_error=%s session=%s samples=%llu drops=%llu err=%s cadence_available=%u timer_ticks=%llu conversion_attempts=%llu collapsed_periods=%llu",
                seismic_recorder_stop_stage_name(final_snapshot.stop_stage),
                seismic_recorder_stop_stage_name(final_snapshot.stop_error_stage),
                esp_err_to_name(final_snapshot.stop_stage_error),
                final_snapshot.session_id,
                (unsigned long long)final_snapshot.samples_written,
                (unsigned long long)final_snapshot.dropped_frames,
                esp_err_to_name(final_snapshot.last_error),
                final_snapshot.cadence.available ? 1U : 0U,
                (unsigned long long)final_snapshot.cadence.timer_ticks,
                (unsigned long long)final_snapshot.cadence.conversion_attempts,
                (unsigned long long)final_snapshot.cadence.collapsed_periods
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

    /* This is the bounded ~30 s durability checkpoint. Normal batch writes
     * remain asynchronous; only the periodic checkpoint forces media sync. */
    const esp_err_t err = seismic_recorder_fs_flush(file, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Periodic SD checkpoint failed: %s", esp_err_to_name(err));
        seismic_recorder_record_stop_error(
            SEISMIC_RECORDER_STOP_STAGE_FLUSH,
            err,
            SEISMIC_RECORDER_STOP_STAGE_SYNC
        );
    }
}


static esp_err_t seismic_recorder_cache_info_locked(
    seismic_recorder_cache_info_t *info
)
{
    return seismic_recorder_cache_get_info(&s_cache, info);
}


static esp_err_t seismic_recorder_flush_one_dirty_chunk(
    FILE *file,
    bool *did_work
)
{
    if (file == NULL || did_work == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *did_work = false;

    size_t dirty_count = 0U;
    uint64_t first_session_frame = 0U;

    seismic_recorder_lock();
    esp_err_t err = seismic_recorder_cache_copy_dirty(
        &s_cache,
        s_batch,
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES,
        &dirty_count,
        &first_session_frame
    );
    seismic_recorder_unlock();
    if (err != ESP_OK || dirty_count == 0U) {
        return err;
    }

    size_t frames_written = 0U;
    const esp_err_t write_err = seismic_recorder_fs_write_frames(
        file,
        s_batch,
        dirty_count,
        &frames_written
    );

    uint64_t last_source_frame = 0U;
    if (frames_written > 0U) {
        seismic_recorder_lock();
        err = seismic_recorder_cache_mark_persisted(
            &s_cache,
            first_session_frame,
            frames_written,
            &last_source_frame
        );
        if (err == ESP_OK && s_snapshot.running && file == s_samples_file) {
            s_cursor = last_source_frame;
            s_snapshot.samples_written += frames_written;
        }
        seismic_recorder_unlock();
        if (err != ESP_OK) {
            return err;
        }
        *did_work = true;
    }

    if (write_err != ESP_OK) {
        return write_err;
    }
    if (frames_written != dirty_count) {
        return ESP_FAIL;
    }

    return ESP_OK;
}


static void seismic_recorder_task(void *argument)
{
    (void)argument;

    for (;;) {
        FILE *file = NULL;
        uint64_t persisted_cursor = 0U;
        uint64_t ingest_cursor = 0U;
        uint64_t stop_target = 0U;
        bool running = false;
        bool stop_requested = false;
        seismic_recorder_stop_stage_t stop_stage;
        seismic_recorder_cache_info_t cache_info = {0};

        seismic_recorder_lock();
        running = s_snapshot.running;
        file = s_samples_file;
        persisted_cursor = s_cursor;
        ingest_cursor = s_ingest_cursor;
        stop_requested = s_stop_requested;
        stop_target = s_stop_target;
        stop_stage = s_snapshot.stop_stage;
        const esp_err_t cache_info_err =
            seismic_recorder_cache_info_locked(&cache_info);
        seismic_recorder_unlock();

        if (!running) {
            vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_IDLE_DELAY_MS));
            seismic_recorder_observe();
            continue;
        }

        if (cache_info_err != ESP_OK) {
            seismic_recorder_record_stop_error(
                SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                cache_info_err,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            seismic_recorder_observe();
            continue;
        }

        /* After drain completion execute one explicit finalization stage per
         * turn. Error shutdown may intentionally bypass dirty RAM data; normal
         * STOP reaches here only after dirty_frames is zero. */
        if (stop_requested && stop_stage != SEISMIC_RECORDER_STOP_STAGE_DRAIN) {
            seismic_recorder_stop_step();
            seismic_recorder_observe();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (
            stop_requested &&
            ingest_cursor >= stop_target &&
            cache_info.dirty_frames == 0U
        ) {
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

        bool did_ingest = false;
        bool cache_full = false;
        seismic_buffer_info_t info = {0};
        uint64_t reported_dropped = 0U;

        /* First rescue new samples from the small realtime ring into PSRAM.
         * No SD/SPI2 operation happens on this path. */
        if (!stop_requested || ingest_cursor < stop_target) {
            uint64_t copy_cursor = ingest_cursor;
            size_t frame_count = 0U;
            uint64_t dropped = 0U;

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

            size_t allowed_count = frame_count;
            uint64_t effective_dropped = dropped;
            const uint64_t first_frame =
                frame_count > 0U
                    ? copy_cursor - (uint64_t)frame_count + 1U
                    : ingest_cursor + dropped + 1U;

            if (stop_requested) {
                const uint64_t remaining_to_target =
                    stop_target > ingest_cursor
                        ? stop_target - ingest_cursor
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

            esp_err_t append_err = ESP_OK;
            if (allowed_count > 0U) {
                seismic_recorder_lock();
                append_err = seismic_recorder_cache_append(
                    &s_cache,
                    first_frame,
                    s_batch,
                    allowed_count
                );
                if (append_err == ESP_OK && s_snapshot.running) {
                    s_ingest_cursor =
                        first_frame + (uint64_t)allowed_count - 1U;
                    s_snapshot.dropped_frames += effective_dropped;
                    did_ingest = true;
                }
                seismic_recorder_unlock();
            }
            else if (effective_dropped > 0U) {
                seismic_recorder_lock();
                if (s_snapshot.running) {
                    s_ingest_cursor += effective_dropped;
                    s_snapshot.dropped_frames += effective_dropped;
                    did_ingest = true;
                }
                seismic_recorder_unlock();
            }

            if (append_err == ESP_ERR_NO_MEM) {
                /* Never overwrite dirty data. Flush one old dirty chunk, then
                 * retry the realtime-ring copy from the unchanged cursor. */
                cache_full = true;
            }
            else if (append_err != ESP_OK) {
                seismic_recorder_record_stop_error(
                    SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                    append_err,
                    SEISMIC_RECORDER_STOP_STAGE_FLUSH
                );
                seismic_recorder_observe();
                continue;
            }

            reported_dropped = dropped;
        }
        else {
            /* Still publish useful backlog diagnostics while flushing STOP. */
            seismic_frame_t scratch;
            size_t scratch_count = 0U;
            (void)seismic_buffer_copy_latest(
                &scratch,
                1U,
                &scratch_count,
                &info
            );
        }

        seismic_recorder_lock();
        persisted_cursor = s_cursor;
        ingest_cursor = s_ingest_cursor;
        esp_err_t current_cache_err =
            seismic_recorder_cache_info_locked(&cache_info);
        seismic_recorder_unlock();
        if (current_cache_err != ESP_OK) {
            seismic_recorder_record_stop_error(
                SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                current_cache_err,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            seismic_recorder_observe();
            continue;
        }

        const size_t target_batch =
            seismic_recorder_target_batch_frames(
                info.sample_rate_hz > 0U
                    ? info.sample_rate_hz
                    : s_snapshot.sample_rate_hz
            );

        const bool near_capacity =
            cache_info.capacity_frames > SEISMIC_RECORDER_BATCH_FRAMES &&
            cache_info.dirty_frames >=
                cache_info.capacity_frames - SEISMIC_RECORDER_BATCH_FRAMES;
        const bool should_flush =
            cache_info.dirty_frames > 0U &&
            (
                stop_requested ||
                cache_full ||
                near_capacity ||
                cache_info.dirty_frames >= target_batch
            );

        bool did_write = false;
        esp_err_t write_err = ESP_OK;
        if (should_flush) {
            write_err = seismic_recorder_flush_one_dirty_chunk(
                file,
                &did_write
            );
            if (write_err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "SD write-behind flush failed: %s",
                    esp_err_to_name(write_err)
                );
                seismic_recorder_record_stop_error(
                    SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                    write_err,
                    SEISMIC_RECORDER_STOP_STAGE_FLUSH
                );
                seismic_recorder_observe();
                continue;
            }
        }

        if (did_write) {
            seismic_recorder_flush_if_due(file);
        }

        bool batch_log_due = false;
        uint64_t total_dropped = 0U;
        uint64_t samples_written = 0U;
        seismic_recorder_lock();
        persisted_cursor = s_cursor;
        ingest_cursor = s_ingest_cursor;
        total_dropped = s_snapshot.dropped_frames;
        samples_written = s_snapshot.samples_written;
        current_cache_err = seismic_recorder_cache_info_locked(&cache_info);
        const int64_t now_us = esp_timer_get_time();
        if (
            current_cache_err == ESP_OK &&
            (did_ingest || did_write || reported_dropped > 0U) &&
            (
                s_last_batch_log_us == 0 ||
                now_us - s_last_batch_log_us >= 1000000LL
            )
        ) {
            s_last_batch_log_us = now_us;
            batch_log_due = true;
        }
        seismic_recorder_unlock();

        if (current_cache_err != ESP_OK) {
            seismic_recorder_record_stop_error(
                SEISMIC_RECORDER_STOP_STAGE_DRAIN,
                current_cache_err,
                SEISMIC_RECORDER_STOP_STAGE_FLUSH
            );
            seismic_recorder_observe();
            continue;
        }

        if (batch_log_due) {
            const uint64_t producer_backlog =
                info.total_frames > ingest_cursor
                    ? info.total_frames - ingest_cursor
                    : 0U;
            ESP_LOGI(
                TAG,
                "RECORDER_CACHE persisted_cursor=%llu ingest_cursor=%llu producer_total=%llu producer_backlog=%llu resident=%u dirty=%u samples_written=%llu dropped_now=%llu total_dropped=%llu stop=%u",
                (unsigned long long)persisted_cursor,
                (unsigned long long)ingest_cursor,
                (unsigned long long)info.total_frames,
                (unsigned long long)producer_backlog,
                (unsigned)cache_info.resident_frames,
                (unsigned)cache_info.dirty_frames,
                (unsigned long long)samples_written,
                (unsigned long long)reported_dropped,
                (unsigned long long)total_dropped,
                stop_requested ? 1U : 0U
            );
        }

        if (did_ingest || did_write) {
            seismic_recorder_observe();
            taskYIELD();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SEISMIC_RECORDER_BATCH_WAIT_MS));
        seismic_recorder_observe();
    }
}


esp_err_t seismic_recorder_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    s_batch = heap_caps_malloc(
        sizeof(*s_batch) * SEISMIC_RECORDER_BATCH_FRAMES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_batch == NULL) {
        ESP_LOGE(
            TAG,
            "Unable to allocate %u-frame recorder batch in PSRAM",
            (unsigned)SEISMIC_RECORDER_BATCH_FRAMES
        );
        return ESP_ERR_NO_MEM;
    }

    const size_t requested_cache_bytes =
        (size_t)CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB * 1024U;
    s_cache_capacity =
        requested_cache_bytes / sizeof(seismic_recorder_cache_slot_t);
    if (s_cache_capacity < SEISMIC_RECORDER_BATCH_FRAMES) {
        heap_caps_free(s_batch);
        s_batch = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    s_cache_storage = heap_caps_malloc(
        s_cache_capacity * sizeof(*s_cache_storage),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_cache_storage == NULL) {
        ESP_LOGE(
            TAG,
            "Unable to allocate recorder write-behind cache: requested=%u KiB",
            (unsigned)CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB
        );
        heap_caps_free(s_batch);
        s_batch = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_cache_init(
        &s_cache,
        s_cache_storage,
        s_cache_capacity
    );
    if (err != ESP_OK) {
        heap_caps_free(s_cache_storage);
        heap_caps_free(s_batch);
        s_cache_storage = NULL;
        s_batch = NULL;
        s_cache_capacity = 0U;
        return err;
    }

    if (!storage_manager_is_ready()) {
        heap_caps_free(s_cache_storage);
        heap_caps_free(s_batch);
        s_cache_storage = NULL;
        s_batch = NULL;
        s_cache_capacity = 0U;
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        heap_caps_free(s_cache_storage);
        heap_caps_free(s_batch);
        s_cache_storage = NULL;
        s_batch = NULL;
        s_cache_capacity = 0U;
        return ESP_ERR_NO_MEM;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        heap_caps_free(s_cache_storage);
        heap_caps_free(s_batch);
        s_cache_storage = NULL;
        s_batch = NULL;
        s_cache_capacity = 0U;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_profile, 0, sizeof(s_profile));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_cadence_start, 0, sizeof(s_cadence_start));
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
        heap_caps_free(s_cache_storage);
        heap_caps_free(s_batch);
        s_cache_storage = NULL;
        s_batch = NULL;
        s_cache_capacity = 0U;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Recorder ready on core %d; batch=%u frames, write-behind=%u frames/%u KiB, write threshold=%u ms",
        APP_TASK_CORE_SERVICES,
        (unsigned)SEISMIC_RECORDER_BATCH_FRAMES,
        (unsigned)s_cache_capacity,
        (unsigned)CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB,
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
        else if (!acquisition.latest_valid || acquisition.samples == 0U) {
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

    ESP_LOGI(TAG, "Preparing session for object=%s", object_id);

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

    snprintf(initial_snapshot.object_id, sizeof(initial_snapshot.object_id), "%s", object_id);
    snprintf(initial_snapshot.session_id, sizeof(initial_snapshot.session_id), "%s", session_id);
    snprintf(
        initial_snapshot.source_name,
        sizeof(initial_snapshot.source_name),
        "%s",
        acquisition.source_name != NULL ? acquisition.source_name : "UNKNOWN"
    );

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

    /* Session setup may itself use SD. Capture the scheduler baseline directly
     * before the source cursor census so the cadence evidence is scoped to the
     * same recording boundary. Missing evidence never becomes a synthetic 0. */
    seismic_acquisition_snapshot_t boundary_acquisition = {0};
    seismic_recorder_cadence_t cadence_start = {0};
    if (seismic_acquisition_get_snapshot(&boundary_acquisition) == ESP_OK) {
        cadence_start = seismic_recorder_cadence_snapshot(&boundary_acquisition);
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

    seismic_recorder_cache_reset(&s_cache);
    s_snapshot = initial_snapshot;
    s_cadence_start = cadence_start;
    s_profile = *profile;
    s_samples_file = file;
    s_ingest_cursor = buffer_info.total_frames;
    s_cursor = buffer_info.total_frames;
    s_stop_target = s_ingest_cursor;
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
        "Recording %s/%s at %lu Hz from source cursor %llu; PSRAM cache=%u frames, SD threshold=%u frames",
        object_id,
        session_id,
        (unsigned long)acquisition.sample_rate_hz,
        (unsigned long long)buffer_info.total_frames,
        (unsigned)s_cache_capacity,
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

    /* Read the cumulative cadence immediately after the stop census. The
     * recorder worker has not been told to finalize yet, so the delta is stored
     * before metadata can be committed. A few extra scheduler ticks can only
     * widen the evidence window; they cannot hide a collapsed period. */
    seismic_acquisition_snapshot_t boundary_acquisition = {0};
    seismic_recorder_cadence_t cadence_end = {0};
    if (seismic_acquisition_get_snapshot(&boundary_acquisition) == ESP_OK) {
        cadence_end = seismic_recorder_cadence_snapshot(&boundary_acquisition);
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

    s_stop_requested = true;
    s_snapshot.state = SEISMIC_RECORDER_STOPPING;
    s_snapshot.stop_stage = SEISMIC_RECORDER_STOP_STAGE_DRAIN;
    s_snapshot.stop_error_stage = SEISMIC_RECORDER_STOP_STAGE_NONE;
    s_snapshot.stop_stage_error = ESP_OK;
    s_stop_target =
        info_err == ESP_OK && info.total_frames >= s_ingest_cursor
            ? info.total_frames
            : s_ingest_cursor;
    seismic_recorder_cadence_delta(
        &s_cadence_start,
        &cadence_end,
        &s_snapshot.cadence
    );
    const uint64_t target = s_stop_target;
    const uint64_t ingest_cursor = s_ingest_cursor;
    const uint64_t persisted_cursor = s_cursor;
    const seismic_recorder_cadence_t cadence = s_snapshot.cadence;
    seismic_recorder_unlock();

    const uint64_t backlog =
        info.total_frames > ingest_cursor
            ? info.total_frames - ingest_cursor
            : 0U;
    ESP_LOGI(
        TAG,
        "STOP_REQUEST stage=drain persisted=%llu ingest=%llu target=%llu producer_total=%llu buffered=%u producer_backlog=%llu info_err=%s cadence_available=%u collapsed_periods=%llu",
        (unsigned long long)persisted_cursor,
        (unsigned long long)ingest_cursor,
        (unsigned long long)target,
        (unsigned long long)info.total_frames,
        (unsigned)info.buffered_frames,
        (unsigned long long)backlog,
        esp_err_to_name(info_err),
        cadence.available ? 1U : 0U,
        (unsigned long long)cadence.collapsed_periods
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


bool seismic_recorder_cache_contains_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    uint64_t frame_count
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        s_mutex == NULL
    ) {
        return false;
    }

    seismic_recorder_lock();
    const bool matches =
        strcmp(s_snapshot.object_id, object_id) == 0 &&
        strcmp(s_snapshot.session_id, session_id) == 0 &&
        seismic_recorder_cache_contains_range(
            &s_cache,
            first_session_frame,
            frame_count
        );
    seismic_recorder_unlock();
    return matches;
}


esp_err_t seismic_recorder_cache_copy_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        frames == NULL ||
        frame_count == 0U ||
        s_mutex == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    seismic_recorder_lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (
        strcmp(s_snapshot.object_id, object_id) == 0 &&
        strcmp(s_snapshot.session_id, session_id) == 0
    ) {
        err = seismic_recorder_cache_copy_range(
            &s_cache,
            first_session_frame,
            frames,
            frame_count
        );
    }
    seismic_recorder_unlock();
    return err;
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
