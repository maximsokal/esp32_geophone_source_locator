#include "localization_job.h"

#include <string.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


static const char *TAG = "localization_job";

static localization_job_state_t s_state;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_wake;
static TaskHandle_t s_task;

/* The worker's own copies (not on its stack). */
static localization_calculate_config_t s_config;
static localization_calculate_summary_t s_summary;


static void progress_hook(
    const char *method_name,
    bool completed,
    size_t methods_completed,
    size_t methods_total,
    void *context
)
{
    (void)context;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    localization_job_state_progress(
        &s_state, method_name, completed, methods_completed, methods_total, esp_timer_get_time()
    );
    xSemaphoreGive(s_mutex);
}


static void localization_job_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)xSemaphoreTake(s_wake, portMAX_DELAY);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        const bool started = localization_job_state_start(&s_state, esp_timer_get_time(), &s_config);
        char object_id[LOCALIZATION_JOB_ID_SIZE];
        char session_id[LOCALIZATION_JOB_ID_SIZE];
        memcpy(object_id, s_state.status.object_id, sizeof(object_id));
        memcpy(session_id, s_state.status.session_id, sizeof(session_id));
        const uint64_t computed_at_ms = s_state.status.computed_at_ms;
        const uint32_t job_id = s_state.status.job_id;
        xSemaphoreGive(s_mutex);
        if (!started) {
            continue;
        }

        s_config.progress = progress_hook;
        s_config.progress_context = NULL;
        memset(&s_summary, 0, sizeof(s_summary));
        ESP_LOGI(TAG, "job %u: %s/%s started", (unsigned)job_id, object_id, session_id);
        const esp_err_t result = localization_calculate_run(
            object_id, session_id, &s_config, computed_at_ms, &s_summary
        );

        /* ESP-IDF reports the high-water mark in bytes. */
        const uint32_t stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        localization_job_state_finish(&s_state, result, &s_summary, stack_free, esp_timer_get_time());
        const localization_job_status_t status = s_state.status;
        xSemaphoreGive(s_mutex);

        ESP_LOGI(
            TAG, "job %u: %s in %u ms, %u/%u methods solved (%u planned), %u skipped for time, participating=%u, err=%s, "
                 "STACK_HWM free=%u of %u",
            (unsigned)job_id, localization_job_state_name(status.state), (unsigned)status.elapsed_ms,
            (unsigned)status.summary.methods_solved, (unsigned)status.summary.methods_attempted,
            (unsigned)status.summary.methods_total, (unsigned)status.summary.methods_skipped_for_time,
            (unsigned)status.summary.participating_channel_count, esp_err_to_name(status.error),
            (unsigned)status.stack_free_min_bytes, (unsigned)status.stack_size_bytes
        );
        if (status.summary.methods_solved == 0U) {
            /* "0/1 solved" says nothing by itself: attempted counts only methods that ran. The reason is in the
             * LOC_SEARCH / LOC_REJECT / LOC_PICK / LOC_METHOD lines the calculation printed just before. */
            ESP_LOGW(TAG, "job %u: nothing solved; attempted=%u of %u planned: the calculation stopped early or every method was rejected, see LOC_* lines above",
                     (unsigned)job_id, (unsigned)status.summary.methods_attempted, (unsigned)status.summary.methods_total);
        }
        for (size_t i = 0U; i < status.summary.timing_count; ++i) {
            ESP_LOGI(TAG, "  %-28s %6u ms%s", status.summary.timings[i].method_name,
                     (unsigned)status.summary.timings[i].duration_ms,
                     status.summary.timings[i].skipped_for_time ? " (skipped: time budget)" : "");
        }
        if (!status.stack_margin_ok) {
            ESP_LOGW(TAG, "worker stack margin below %u bytes: raise LOCALIZATION_JOB_STACK_BYTES",
                     (unsigned)LOCALIZATION_JOB_STACK_MARGIN_BYTES);
        }
    }
}


esp_err_t localization_job_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    localization_job_state_init(&s_state, LOCALIZATION_JOB_STACK_BYTES);
    s_mutex = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (s_mutex == NULL || s_wake == NULL) {
        if (s_wake != NULL) vSemaphoreDelete(s_wake);
        if (s_mutex != NULL) vSemaphoreDelete(s_mutex);
        s_wake = NULL;
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    /*
     * Core 1 ("signal-processing/localization work", app_task_config.h) at
     * the idle priority: acquisition always preempts it, and it shares
     * ticks with IDLE1 (which yields to it), so the task watchdog is fed
     * however long a run takes. The stack lives in PSRAM.
     */
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        localization_job_task,
        "localization_job",
        LOCALIZATION_JOB_STACK_BYTES,
        NULL,
        APP_TASK_PRIORITY_LOCALIZATION_JOB,
        &s_task,
        APP_TASK_CORE_REALTIME,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created != pdPASS) {
        s_task = NULL;
        vSemaphoreDelete(s_wake);
        vSemaphoreDelete(s_mutex);
        s_wake = NULL;
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "worker core=%d stack=%u (spiram)", APP_TASK_CORE_REALTIME, (unsigned)LOCALIZATION_JOB_STACK_BYTES);
    return ESP_OK;
}


esp_err_t localization_job_submit(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint32_t *job_id_out
)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const esp_err_t err = localization_job_state_submit(
        &s_state, object_id, session_id, config, budget_ms, (uint64_t)(esp_timer_get_time() / 1000)
    );
    if (err == ESP_OK && job_id_out != NULL) {
        *job_id_out = s_state.status.job_id;
    }
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK) {
        xSemaphoreGive(s_wake);
    }
    return err;
}


void localization_job_get_status(localization_job_status_t *status_out)
{
    if (status_out == NULL) {
        return;
    }
    if (s_mutex == NULL) {
        memset(status_out, 0, sizeof(*status_out));
        status_out->state = LOCALIZATION_JOB_IDLE;
        status_out->stack_size_bytes = LOCALIZATION_JOB_STACK_BYTES;
        status_out->stack_margin_ok = true;
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    localization_job_state_snapshot(&s_state, esp_timer_get_time(), status_out);
    xSemaphoreGive(s_mutex);
}


esp_err_t localization_job_status_json(char **json_out)
{
    localization_job_status_t status;
    localization_job_get_status(&status);
    return localization_job_status_to_json(&status, json_out);
}
