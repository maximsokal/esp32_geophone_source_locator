#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "localization_job.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static unsigned s_sem_create_calls;
static unsigned s_sem_delete_calls;
static unsigned s_task_create_calls;
static unsigned s_fail_sem_create_at;
static bool s_fail_task_create;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    ++s_sem_create_calls;
    if (s_sem_create_calls == s_fail_sem_create_at) return NULL;
    return malloc(1U);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    ++s_sem_create_calls;
    if (s_sem_create_calls == s_fail_sem_create_at) return NULL;
    return malloc(1U);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)semaphore;
    (void)timeout;
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore != NULL) {
        ++s_sem_delete_calls;
        free(semaphore);
    }
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    void (*task)(void *),
    const char *name,
    uint32_t stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    BaseType_t core_id,
    unsigned memory_caps
)
{
    (void)task;
    (void)name;
    (void)stack_size;
    (void)argument;
    (void)priority;
    (void)core_id;
    (void)memory_caps;
    ++s_task_create_calls;
    if (s_fail_task_create) return pdFALSE;
    *task_handle = (TaskHandle_t)(uintptr_t)1U;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }
uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 0U; }
int64_t esp_timer_get_time(void) { return 0; }

void localization_job_state_init(localization_job_state_t *state, uint32_t stack_size_bytes)
{
    (void)state;
    (void)stack_size_bytes;
}

esp_err_t localization_job_state_submit(
    localization_job_state_t *state,
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint64_t computed_at_ms)
{
    (void)state; (void)object_id; (void)session_id; (void)config;
    (void)budget_ms; (void)computed_at_ms;
    return ESP_OK;
}

bool localization_job_state_start(
    localization_job_state_t *state,
    int64_t now_us,
    localization_calculate_config_t *config_out)
{
    (void)state; (void)now_us; (void)config_out;
    return false;
}

void localization_job_state_progress(
    localization_job_state_t *state,
    const char *method_name,
    bool completed,
    size_t methods_completed,
    size_t methods_total,
    int64_t now_us)
{
    (void)state; (void)method_name; (void)completed;
    (void)methods_completed; (void)methods_total; (void)now_us;
}

void localization_job_state_finish(
    localization_job_state_t *state,
    esp_err_t result,
    const localization_calculate_summary_t *summary,
    uint32_t stack_free_min_bytes,
    int64_t now_us)
{
    (void)state; (void)result; (void)summary;
    (void)stack_free_min_bytes; (void)now_us;
}

void localization_job_state_snapshot(
    const localization_job_state_t *state,
    int64_t now_us,
    localization_job_status_t *status_out)
{
    (void)now_us;
    if (state != NULL && status_out != NULL) *status_out = state->status;
}

const char *localization_job_state_name(localization_job_state_id_t state)
{
    (void)state;
    return "idle";
}

esp_err_t localization_job_status_to_json(
    const localization_job_status_t *status,
    char **json_out)
{
    (void)status; (void)json_out;
    return ESP_OK;
}

esp_err_t localization_calculate_run(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary_out)
{
    (void)object_id; (void)session_id; (void)config;
    (void)computed_at_ms; (void)summary_out;
    return ESP_OK;
}

static void test_partial_allocation_and_task_create_failures_can_retry(void)
{
    s_fail_sem_create_at = 2U;
    assert(localization_job_init() == ESP_ERR_NO_MEM);
    assert(s_sem_create_calls == 2U);
    assert(s_sem_delete_calls == 1U);

    s_fail_sem_create_at = 0U;
    s_fail_task_create = true;
    assert(localization_job_init() == ESP_ERR_NO_MEM);
    assert(s_sem_create_calls == 4U);
    assert(s_sem_delete_calls == 3U);
    assert(s_task_create_calls == 1U);

    s_fail_task_create = false;
    assert(localization_job_init() == ESP_OK);
    assert(s_sem_create_calls == 6U);
    assert(s_sem_delete_calls == 3U);
    assert(s_task_create_calls == 2U);
}

int main(void)
{
    test_partial_allocation_and_task_create_failures_can_retry();
    puts("LOCALIZATION_JOB_INIT_FAULT_PASS");
    return 0;
}
