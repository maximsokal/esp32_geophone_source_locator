#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "status_indicator.h"
#include "app_events.h"
#include "esp_event.h"

const char *APP_EVENT = "APP_EVENT";

static int s_loop_token;
static esp_event_loop_handle_t s_loop;
static esp_err_t s_register_result = ESP_OK;
static unsigned s_register_calls;
static unsigned s_unregister_calls;
static esp_event_handler_t s_handler;
static app_event_type_t s_posted_event;
static int32_t s_posted_value;

esp_event_loop_handle_t app_events_get_loop(void)
{
    return s_loop;
}

esp_err_t app_events_post(app_event_type_t type, int32_t value)
{
    s_posted_event = type;
    s_posted_value = value;
    return ESP_OK;
}

esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *arg)
{
    (void)arg;
    assert(loop == &s_loop_token);
    assert(base == APP_EVENT);
    assert(id == ESP_EVENT_ANY_ID);
    ++s_register_calls;
    if (s_register_result == ESP_OK) {
        s_handler = handler;
    }
    return s_register_result;
}

esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id, esp_event_handler_t handler)
{
    assert(loop == &s_loop_token);
    assert(base == APP_EVENT);
    assert(id == ESP_EVENT_ANY_ID);
    assert(handler == s_handler);
    ++s_unregister_calls;
    return ESP_OK;
}

static void emit(app_event_type_t event)
{
    assert(s_handler != NULL);
    s_handler(NULL, APP_EVENT, event, NULL);
}

int main(void)
{
    assert(status_indicator_get_state() == STATUS_INDICATOR_NORMAL);
    assert(!status_indicator_backend_enabled());

    s_loop = NULL;
    assert(status_indicator_init() == ESP_ERR_INVALID_STATE);

    s_loop = &s_loop_token;
    s_register_result = ESP_FAIL;
    assert(status_indicator_init() == ESP_FAIL);
    assert(s_register_calls == 1U);

    s_register_result = ESP_OK;
    assert(status_indicator_init() == ESP_OK);
    assert(s_register_calls == 2U);
    assert(status_indicator_init() == ESP_OK);
    assert(s_register_calls == 2U);

    assert(status_indicator_post(APP_STATUS_WARNING) == ESP_OK);
    assert(s_posted_event == APP_EVENT_STATUS_WARNING && s_posted_value == 0);
    emit(APP_EVENT_STATUS_WARNING);
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);

    assert(status_indicator_post(APP_STATUS_NORMAL) == ESP_OK);
    assert(s_posted_event == APP_EVENT_STATUS_CLEAR_NON_STICKY);
    emit(APP_EVENT_STATUS_CLEAR_NON_STICKY);
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);

    emit(APP_EVENT_STATUS_ERROR);
    assert(status_indicator_get_state() == STATUS_INDICATOR_ERROR);
    emit(APP_EVENT_STATUS_WARNING);
    assert(status_indicator_get_state() == STATUS_INDICATOR_ERROR);

    assert(status_indicator_post((app_status_event_t)99) == ESP_ERR_INVALID_ARG);

    status_indicator_stop();
    assert(s_unregister_calls == 1U);
    status_indicator_stop();
    assert(s_unregister_calls == 1U);

    assert(status_indicator_init() == ESP_OK);
    assert(s_register_calls == 3U);
    assert(status_indicator_get_state() == STATUS_INDICATOR_ERROR);

    return 0;
}
