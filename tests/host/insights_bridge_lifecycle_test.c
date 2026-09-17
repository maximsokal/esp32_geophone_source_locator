#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "insights_bridge.h"
#include "app_events.h"
#include "app_task_config.h"
#include "esp_insights.h"
#include "freertos/task.h"

const char *APP_EVENT = "APP_EVENT";
static const char *OTHER_EVENT = "OTHER_EVENT";

static int s_loop_token;
static int s_task_token;
static bool s_fail_worker_create;
static bool s_fail_handler_register;
static bool s_fail_handler_unregister;
static bool s_run_worker_on_delay;
static bool s_worker_running;
static esp_err_t s_insights_result = ESP_FAIL;

static unsigned s_worker_create_calls;
static unsigned s_handler_register_calls;
static unsigned s_handler_unregister_calls;
static unsigned s_insights_init_calls;
static unsigned s_insights_deinit_calls;
static unsigned s_notify_give_calls;
static unsigned s_notify_take_calls;
static unsigned s_task_delete_calls;
static unsigned s_delay_calls;
static TaskFunction_t s_worker_entry;
static esp_event_handler_t s_event_handler;

esp_event_loop_handle_t app_events_get_loop(void)
{
    return &s_loop_token;
}

esp_err_t esp_event_handler_register_with(
    esp_event_loop_handle_t loop,
    esp_event_base_t base,
    int32_t id,
    esp_event_handler_t handler,
    void *arg)
{
    (void)arg;
    assert(loop == &s_loop_token);
    assert(base == APP_EVENT);
    assert(id == APP_EVENT_WIFI_STA_CONNECTED);
    ++s_handler_register_calls;
    if (s_fail_handler_register) {
        return ESP_FAIL;
    }
    s_event_handler = handler;
    return ESP_OK;
}

esp_err_t esp_event_handler_unregister_with(
    esp_event_loop_handle_t loop,
    esp_event_base_t base,
    int32_t id,
    esp_event_handler_t handler)
{
    assert(loop == &s_loop_token);
    assert(base == APP_EVENT);
    assert(id == APP_EVENT_WIFI_STA_CONNECTED);
    assert(handler == s_event_handler);
    ++s_handler_unregister_calls;
    if (s_fail_handler_unregister) {
        return ESP_FAIL;
    }
    s_event_handler = NULL;
    return ESP_OK;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core)
{
    (void)arg;
    ++s_worker_create_calls;
    assert(strcmp(name, "insights_retry") == 0);
    assert(stack == APP_TASK_STACK_INSIGHTS);
    assert(priority == APP_TASK_PRIORITY_INSIGHTS);
    assert(core == APP_TASK_CORE_SERVICES);
    if (s_fail_worker_create) {
        return pdFALSE;
    }
    s_worker_entry = task;
    *handle = &s_task_token;
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &s_task_token);
    ++s_notify_give_calls;
    return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait_ticks)
{
    assert(clear_on_exit == pdTRUE);
    assert(wait_ticks == portMAX_DELAY);
    ++s_notify_take_calls;
    return 1U;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL);
    ++s_task_delete_calls;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(1U));
    ++s_delay_calls;
    if (s_run_worker_on_delay && s_worker_entry != NULL && !s_worker_running) {
        s_worker_running = true;
        TaskFunction_t entry = s_worker_entry;
        s_worker_entry = NULL;
        entry(NULL);
        s_worker_running = false;
    }
}

esp_err_t esp_insights_init(const esp_insights_config_t *config)
{
    assert(config != NULL);
    assert(config->log_type == 0U);
    ++s_insights_init_calls;
    return s_insights_result;
}

void esp_insights_deinit(void)
{
    ++s_insights_deinit_calls;
}

static void exercise_event_filter(void)
{
    assert(s_event_handler != NULL);
    const unsigned before = s_notify_give_calls;
    s_event_handler(NULL, OTHER_EVENT, APP_EVENT_WIFI_STA_CONNECTED, NULL);
    s_event_handler(NULL, APP_EVENT, APP_EVENT_WIFI_STA_CONNECTED + 1, NULL);
    assert(s_notify_give_calls == before);
    s_event_handler(NULL, APP_EVENT, APP_EVENT_WIFI_STA_CONNECTED, NULL);
    assert(s_notify_give_calls == before + 1U);
}

int main(void)
{
    assert(!insights_bridge_enabled());
    assert(insights_bridge_publish_runtime() == ESP_ERR_NOT_SUPPORTED);

    s_fail_worker_create = true;
    s_fail_handler_register = true;
    s_insights_result = ESP_FAIL;
    assert(insights_bridge_init() == ESP_OK);
    assert(!insights_bridge_enabled());
    assert(s_worker_create_calls == 1U);
    assert(s_handler_register_calls == 1U);
    assert(s_insights_init_calls == 1U);
    insights_bridge_shutdown();
    assert(s_handler_unregister_calls == 0U);
    assert(s_insights_deinit_calls == 0U);

    s_fail_worker_create = false;
    s_fail_handler_register = false;
    s_run_worker_on_delay = true;
    assert(insights_bridge_init() == ESP_OK);
    assert(!insights_bridge_enabled());
    assert(s_worker_create_calls == 2U);
    assert(s_handler_register_calls == 2U);
    assert(s_insights_init_calls == 2U);
    exercise_event_filter();

    const unsigned notified_before_direct = s_notify_give_calls;
    insights_bridge_on_network_ready();
    assert(s_notify_give_calls == notified_before_direct + 1U);

    insights_bridge_shutdown();
    assert(s_handler_unregister_calls == 1U);
    assert(s_event_handler == NULL);
    assert(s_task_delete_calls == 1U);
    assert(s_notify_take_calls == 1U);
    assert(s_insights_deinit_calls == 0U);
    assert(!insights_bridge_enabled());

    const unsigned notify_after_shutdown = s_notify_give_calls;
    insights_bridge_on_network_ready();
    assert(s_notify_give_calls == notify_after_shutdown);
    insights_bridge_shutdown();
    assert(s_handler_unregister_calls == 1U);
    assert(s_task_delete_calls == 1U);
    assert(s_insights_deinit_calls == 0U);

    s_insights_result = ESP_OK;
    assert(insights_bridge_init() == ESP_OK);
    assert(insights_bridge_enabled());
    assert(s_worker_create_calls == 3U);
    assert(s_handler_register_calls == 3U);
    assert(s_insights_init_calls == 3U);

    const unsigned worker_calls = s_worker_create_calls;
    const unsigned handler_calls = s_handler_register_calls;
    const unsigned init_calls = s_insights_init_calls;
    assert(insights_bridge_init() == ESP_OK);
    assert(s_worker_create_calls == worker_calls);
    assert(s_handler_register_calls == handler_calls);
    assert(s_insights_init_calls == init_calls);

    exercise_event_filter();
    insights_bridge_shutdown();
    assert(s_handler_unregister_calls == 2U);
    assert(s_task_delete_calls == 2U);
    assert(s_insights_deinit_calls == 1U);
    assert(!insights_bridge_enabled());

    insights_bridge_shutdown();
    assert(s_handler_unregister_calls == 2U);
    assert(s_task_delete_calls == 2U);
    assert(s_insights_deinit_calls == 1U);
    assert(s_delay_calls >= 2U);
    return 0;
}
