#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"

static esp_event_loop_args_t s_args;
static int s_create_calls;
static int s_post_calls;
static esp_event_loop_handle_t s_loop = (void *)0x1234;
static esp_event_loop_handle_t s_post_loop;
static esp_event_base_t s_post_base;
static int32_t s_post_id;
static int32_t s_post_value;
static uint32_t s_post_timeout;

esp_err_t esp_event_loop_create(
    const esp_event_loop_args_t *args,
    esp_event_loop_handle_t *event_loop
)
{
    s_args = *args;
    *event_loop = s_loop;
    s_create_calls++;
    return ESP_OK;
}

esp_err_t esp_event_post_to(
    esp_event_loop_handle_t event_loop,
    esp_event_base_t event_base,
    int32_t event_id,
    const void *event_data,
    size_t event_data_size,
    uint32_t ticks_to_wait
)
{
    assert(event_data_size == sizeof(s_post_value));
    s_post_loop = event_loop;
    s_post_base = event_base;
    s_post_id = event_id;
    memcpy(&s_post_value, event_data, sizeof(s_post_value));
    s_post_timeout = ticks_to_wait;
    s_post_calls++;
    return ESP_OK;
}

int main(void)
{
    assert(app_events_get_loop() == NULL);
    assert(!app_events_keypad_delivery_enabled());
    assert(app_events_post(APP_EVENT_KEYPAD, 7) == ESP_ERR_INVALID_STATE);
    assert(app_events_init() == ESP_OK);
    assert(app_events_init() == ESP_OK);
    assert(s_create_calls == 1);
    assert(s_args.queue_size == 24);
    assert(s_args.task_core_id == 0);
    assert(s_args.task_priority == 6);
    assert(s_args.task_stack_size == 6144);
    assert(strcmp(s_args.task_name, "app_events") == 0);
    assert(app_events_get_loop() == s_loop);
    assert(!app_events_keypad_delivery_enabled());

    /* Early Button polling is live during boot, but keypad delivery is closed
     * until the interactive UI explicitly arms it. Dropped boot navigation is
     * still a successful best-effort post from the producer's perspective. */
    assert(app_events_post(APP_EVENT_KEYPAD, 5) == ESP_OK);
    assert(s_post_calls == 0);

    app_events_set_keypad_delivery_enabled(true);
    assert(app_events_keypad_delivery_enabled());
    assert(app_events_post(APP_EVENT_KEYPAD, 5) == ESP_OK);
    assert(s_post_calls == 1);
    assert(s_post_id == APP_EVENT_KEYPAD);
    assert(s_post_value == 5);

    app_events_set_keypad_delivery_enabled(false);
    assert(!app_events_keypad_delivery_enabled());
    assert(app_events_post(APP_EVENT_KEYPAD, 4) == ESP_OK);
    assert(s_post_calls == 1);

    assert(app_events_post(APP_EVENT_TLS_VERIFY_RESULT, -42) == ESP_OK);
    assert(s_post_calls == 2);
    assert(s_post_loop == s_loop);
    assert(s_post_base == APP_EVENT);
    assert(s_post_id == APP_EVENT_TLS_VERIFY_RESULT);
    assert(s_post_value == -42);
    assert(s_post_timeout == 0U);

    assert(app_events_post(APP_EVENT_POWER_SHUTDOWN_REQUESTED, 0) == ESP_OK);
    assert(s_post_calls == 3);
    assert(s_post_id == APP_EVENT_POWER_SHUTDOWN_REQUESTED);
    assert(s_post_value == 0);
    assert(s_post_timeout == 0U);

    assert(app_events_post_best_effort(APP_EVENT_POWER_SHUTDOWN_REQUESTED, 0) == ESP_OK);
    assert(s_post_calls == 4);

    /* Remote-client activity: nothing before the first request, then the caller's last millisecond stamp. */
    uint32_t last_request_ms = 123U;
    assert(!app_events_remote_activity_last(&last_request_ms));
    assert(last_request_ms == 123U);                       /* untouched when there is nothing to report */
    app_events_remote_activity_note(0U);
    assert(app_events_remote_activity_last(&last_request_ms));
    assert(last_request_ms == 0U);                         /* a request at ms 0 is still a request */
    app_events_remote_activity_note(41000U);
    assert(app_events_remote_activity_last(&last_request_ms));
    assert(last_request_ms == 41000U);
    assert(app_events_remote_activity_last(NULL));

    puts("app events host contract passed");
    return 0;
}
