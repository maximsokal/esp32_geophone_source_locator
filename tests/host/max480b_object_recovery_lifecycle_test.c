#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "object_recovery.h"
#include "object_recovery_internal.h"

typedef struct {
    bool allocated;
    bool pending_reap;
} fake_task_resource_t;

static fake_task_resource_t s_resources[8];
static TaskFunction_t s_created_task;
static void *s_created_argument;
static fake_task_resource_t *s_latest_resource;
static fake_task_resource_t *s_running_resource;
static bool s_task_running;
static bool s_fail_next_create;
static bool s_run_worker_on_next_delay;
static bool s_request_rescan_on_first_pass;
static unsigned s_create_calls;
static unsigned s_run_calls;
static unsigned s_pending_reaps;
static unsigned s_freed_resources;
static unsigned s_delay_count;
static TickType_t s_delays[8];
static unsigned s_first_callback_calls;
static unsigned s_replacement_callback_calls;
static int s_callback_context_token;

static void run_latest_task(void)
{
    assert(s_latest_resource != NULL && s_created_task != NULL && !s_task_running);
    s_task_running = true;
    s_running_resource = s_latest_resource;
    s_created_task(s_created_argument);
    s_running_resource = NULL;
    s_task_running = false;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_bytes,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *task_out,
    BaseType_t core_id,
    uint32_t caps)
{
    assert(task != NULL && name != NULL && task_out != NULL);
    assert(stack_bytes == APP_TASK_STACK_OBJECT_RECOVERY);
    assert(priority == APP_TASK_PRIORITY_OBJECT_RECOVERY);
    assert(core_id == APP_TASK_CORE_SERVICES);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++s_create_calls;
    if (s_fail_next_create) {
        s_fail_next_create = false;
        return pdFAIL;
    }

    for (size_t index = 0U; index < sizeof(s_resources) / sizeof(s_resources[0]); ++index) {
        if (!s_resources[index].allocated) {
            s_resources[index].allocated = true;
            s_resources[index].pending_reap = false;
            s_latest_resource = &s_resources[index];
            s_created_task = task;
            s_created_argument = argument;
            *task_out = s_latest_resource;
            return pdPASS;
        }
    }
    return pdFAIL;
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    assert(task == NULL && s_task_running && s_running_resource != NULL);
    assert(!s_running_resource->pending_reap);
    s_running_resource->pending_reap = true;
    ++s_pending_reaps;
}

void vTaskDelay(TickType_t ticks_to_wait)
{
    assert(s_delay_count < sizeof(s_delays) / sizeof(s_delays[0]));
    s_delays[s_delay_count++] = ticks_to_wait;
    if (!s_task_running && s_run_worker_on_next_delay) {
        s_run_worker_on_next_delay = false;
        run_latest_task();
    }
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 2048U;
}

static void first_complete(esp_err_t result, void *context)
{
    assert(result == ESP_OK && context == &s_callback_context_token);
    ++s_first_callback_calls;
}

static void replacement_complete(esp_err_t result, void *context)
{
    assert(result == ESP_OK && context == &s_callback_context_token);
    ++s_replacement_callback_calls;
}

esp_err_t object_recovery_run_cooperative(
    object_recovery_stop_requested_fn stop_requested,
    void *user_context,
    bool yield_between_objects)
{
    assert(stop_requested != NULL && user_context == NULL && yield_between_objects);
    assert(!stop_requested(user_context));
    ++s_run_calls;
    if (s_request_rescan_on_first_pass && s_run_calls == 1U) {
        s_request_rescan_on_first_pass = false;
        assert(object_recovery_start_background_with_callback(
            replacement_complete,
            &s_callback_context_token) == ESP_OK);
    }
    return ESP_OK;
}

static void reap_idle_task_resources(void)
{
    for (size_t index = 0U; index < sizeof(s_resources) / sizeof(s_resources[0]); ++index) {
        if (s_resources[index].pending_reap) {
            s_resources[index].allocated = false;
            s_resources[index].pending_reap = false;
            --s_pending_reaps;
            ++s_freed_resources;
        }
    }
}

/* mutation-only: task creation rollback, forced rescan interleaving, retire/recreate and idle reaping. */
static void test_mutation_only_background_worker_lifecycle(void)
{
    s_fail_next_create = true;
    assert(object_recovery_start_background_with_callback(first_complete, &s_callback_context_token) ==
        ESP_ERR_NO_MEM);
    assert(!object_recovery_is_running());
    assert(s_create_calls == 1U);

    s_request_rescan_on_first_pass = true;
    assert(object_recovery_start_background_with_callback(first_complete, &s_callback_context_token) ==
        ESP_OK);
    assert(object_recovery_is_running());
    assert(s_create_calls == 2U);
    run_latest_task();
    assert(!object_recovery_is_running());
    assert(s_run_calls == 2U);
    assert(s_first_callback_calls == 0U);
    assert(s_replacement_callback_calls == 1U);
    assert(s_delay_count == 2U);
    assert(s_delays[0] == pdMS_TO_TICKS(100U));
    assert(s_delays[1] == pdMS_TO_TICKS(250U));
    assert(s_pending_reaps == 1U && s_freed_resources == 0U);

    assert(object_recovery_start_background_with_callback(first_complete, &s_callback_context_token) ==
        ESP_OK);
    assert(s_create_calls == 3U && s_pending_reaps == 1U);
    s_run_worker_on_next_delay = true;
    object_recovery_stop_background();
    assert(!object_recovery_is_running());
    assert(s_run_calls == 2U);
    assert(s_first_callback_calls == 0U);
    assert(s_pending_reaps == 2U && s_freed_resources == 0U);

    assert(object_recovery_start_background() == ESP_OK);
    assert(s_create_calls == 4U && s_pending_reaps == 2U);
    run_latest_task();
    assert(!object_recovery_is_running());
    assert(s_run_calls == 3U);
    assert(s_pending_reaps == 3U && s_freed_resources == 0U);

    reap_idle_task_resources();
    assert(s_pending_reaps == 0U && s_freed_resources == 3U);
}

int main(void)
{
    test_mutation_only_background_worker_lifecycle();
    return 0;
}
