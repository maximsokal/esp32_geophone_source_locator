#include "ui_internal.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_events.h"
#include "ui_event_policy.h"

#include "runtime_observability.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"


static const char *TAG =
    "UI_EVENT";


#define UI_EVENT_PENDING_CAPACITY         24U
#define UI_EVENT_PENDING_DISPATCH_BATCH    8U


static bool s_running =
    false;

static bool s_registered =
    false;

static uint64_t s_handled_events =
    0U;

static uint64_t s_dropped_stale_keypad =
    0U;

typedef struct
{
    int32_t event_id;
    int32_t event_value;
    int64_t queued_us;

} ui_pending_event_t;


static ui_pending_event_t s_pending_events[UI_EVENT_PENDING_CAPACITY];
static size_t s_pending_head = 0U;
static size_t s_pending_tail = 0U;
static size_t s_pending_count = 0U;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;


static bool ui_event_pending_push(
    int32_t event_id,
    int32_t event_value
)
{
    bool accepted = false;
    const int64_t queued_us =
        esp_timer_get_time();

    portENTER_CRITICAL(&s_pending_lock);
    if (s_running && s_pending_count < UI_EVENT_PENDING_CAPACITY) {
        s_pending_events[s_pending_tail] = (ui_pending_event_t) {
            .event_id = event_id,
            .event_value = event_value,
            .queued_us = queued_us,
        };
        s_pending_tail = (s_pending_tail + 1U) % UI_EVENT_PENDING_CAPACITY;
        s_pending_count++;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_pending_lock);

    return accepted;
}


static void ui_event_observe(void)
{
    runtime_observability_beat(
        RUNTIME_TASK_UI,
        s_handled_events
    );
}


static void ui_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;

    if (!s_running) {
        return;
    }

    const int32_t event_value =
        event_data != NULL
            ? *(const int32_t *)event_data
            : 0;

    /*
     * This callback runs on the dedicated APP_LOOP task. It only copies the
     * value into a bounded queue and wakes the existing LVGL task. In
     * particular it never waits for LVGL, SD, TLS, or a service operation.
     */
    if (!ui_event_pending_push(event_id, event_value)) {
        ESP_LOGW(TAG, "UI event queue full, type=%ld", (long)event_id);
        return;
    }

    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
}


void ui_event_dispatcher_dispatch_pending_locked(void)
{
    for (size_t i = 0U; i < UI_EVENT_PENDING_DISPATCH_BATCH; ++i) {
        ui_pending_event_t event;
        bool have_event = false;

        portENTER_CRITICAL(&s_pending_lock);
        if (s_pending_count > 0U) {
            event = s_pending_events[s_pending_head];
            s_pending_head = (s_pending_head + 1U) % UI_EVENT_PENDING_CAPACITY;
            s_pending_count--;
            have_event = true;
        }
        portEXIT_CRITICAL(&s_pending_lock);

        if (!have_event) {
            break;
        }

        bool dispatched = true;
        if (event.event_id == APP_EVENT_KEYPAD) {
            const int64_t now_us =
                esp_timer_get_time();

            if (ui_event_keypad_is_stale(event.queued_us, now_us)) {
                dispatched = false;
                s_dropped_stale_keypad++;

                if (
                    s_dropped_stale_keypad == 1U ||
                    (s_dropped_stale_keypad % 8U) == 0U
                ) {
                    const int64_t age_us =
                        now_us >= event.queued_us
                            ? now_us - event.queued_us
                            : 0;
                    ESP_LOGW(
                        TAG,
                        "Dropped stale keypad navigation age_ms=%lld total=%llu",
                        (long long)(age_us / 1000LL),
                        (unsigned long long)s_dropped_stale_keypad
                    );
                }
            }
            else {
                const int64_t age_us =
                    now_us >= event.queued_us
                        ? now_us - event.queued_us
                        : 0;
                ESP_LOGI(
                    TAG,
                    "KEYPAD_EVENT_DISPATCH key=%ld age_ms=%lld",
                    (long)event.event_value,
                    (long long)(age_us / 1000LL)
                );
                ui_dispatch_key_locked((keypad_key_t)event.event_value);
            }
        }
        else {
            ui_dispatch_app_event_locked(
                event.event_id,
                event.event_value
            );
        }

        /* Consuming a stale input still proves that the UI dispatcher made
         * progress; it simply must not mutate navigation after a long stall. */
        (void)dispatched;
        s_handled_events++;
        ui_event_observe();
    }
}


esp_err_t ui_event_dispatcher_start(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    esp_event_loop_handle_t loop =
        app_events_get_loop();

    if (loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_running =
        true;
    s_handled_events =
        0U;
    s_dropped_stale_keypad =
        0U;

    portENTER_CRITICAL(&s_pending_lock);
    s_pending_head = 0U;
    s_pending_tail = 0U;
    s_pending_count = 0U;
    portEXIT_CRITICAL(&s_pending_lock);

    esp_err_t err =
        esp_event_handler_register_with(
            loop,
            APP_EVENT,
            ESP_EVENT_ANY_ID,
            ui_event_handler,
            NULL
        );

    if (err != ESP_OK) {
        s_running =
            false;
        return err;
    }

    s_registered =
        true;

    ESP_LOGI(
        TAG,
        "Application event subscriber started"
    );

    return ESP_OK;
}


void ui_event_dispatcher_stop(void)
{
    if (!s_registered) {
        return;
    }

    s_running =
        false;

    portENTER_CRITICAL(&s_pending_lock);
    s_pending_head = 0U;
    s_pending_tail = 0U;
    s_pending_count = 0U;
    portEXIT_CRITICAL(&s_pending_lock);

    esp_event_loop_handle_t loop =
        app_events_get_loop();

    if (loop != NULL) {
        (void)esp_event_handler_unregister_with(
            loop,
            APP_EVENT,
            ESP_EVENT_ANY_ID,
            ui_event_handler
        );
    }

    s_registered =
        false;

    ESP_LOGI(
        TAG,
        "Application event subscriber stopped"
    );
}
