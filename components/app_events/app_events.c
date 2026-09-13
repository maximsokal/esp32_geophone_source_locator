#include "app_events.h"

#include <stdbool.h>
#include <stdatomic.h>

#include "app_task_config.h"
#include "esp_err.h"
#include "esp_log.h"


static const char *TAG =
    "APP_EVENTS";


#define APP_EVENTS_QUEUE_SIZE       24
#define APP_EVENTS_TASK_CORE        APP_TASK_CORE_SERVICES
#define APP_EVENTS_TASK_PRIORITY    APP_TASK_PRIORITY_APP_EVENT_LOOP
#define APP_EVENTS_TASK_STACK_SIZE  APP_TASK_STACK_APP_EVENT_LOOP


static esp_event_loop_handle_t s_loop =
    NULL;

static atomic_bool s_keypad_delivery_enabled =
    ATOMIC_VAR_INIT(false);


ESP_EVENT_DEFINE_BASE(APP_EVENT);


esp_err_t app_events_init(void)
{
    if (s_loop != NULL) {
        return ESP_OK;
    }

    const esp_event_loop_args_t loop_args = {
        .queue_size = APP_EVENTS_QUEUE_SIZE,
        .task_name = "app_events",
        .task_priority = APP_EVENTS_TASK_PRIORITY,
        .task_stack_size = APP_EVENTS_TASK_STACK_SIZE,
        .task_core_id = APP_EVENTS_TASK_CORE,
    };

    esp_event_loop_handle_t loop =
        NULL;

    esp_err_t err =
        esp_event_loop_create(
            &loop_args,
            &loop
        );

    if (err != ESP_OK) {
        return err;
    }

    atomic_store_explicit(
        &s_keypad_delivery_enabled,
        false,
        memory_order_release
    );

    s_loop =
        loop;

    ESP_LOGI(
        TAG,
        "Application event loop initialized queue=%d core=%d priority=%d stack=%d",
        APP_EVENTS_QUEUE_SIZE,
        APP_EVENTS_TASK_CORE,
        APP_EVENTS_TASK_PRIORITY,
        APP_EVENTS_TASK_STACK_SIZE
    );

    return ESP_OK;
}


esp_event_loop_handle_t app_events_get_loop(void)
{
    return s_loop;
}


void app_events_set_keypad_delivery_enabled(bool enabled)
{
    atomic_store_explicit(
        &s_keypad_delivery_enabled,
        enabled,
        memory_order_release
    );

    ESP_LOGI(
        TAG,
        "KEYPAD_EVENT_GATE state=%s",
        enabled ? "ARMED" : "DISARMED"
    );
}


static esp_err_t app_events_post_internal(
    app_event_type_t type,
    int32_t value,
    bool log_queue_full
)
{
    if (s_loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        type == APP_EVENT_KEYPAD &&
        !atomic_load_explicit(
            &s_keypad_delivery_enabled,
            memory_order_acquire
        )
    ) {
        /*
         * Button polling intentionally starts early so ADC timing remains
         * deterministic, but navigation is not a boot command. Discard it
         * before it can occupy APP_EVENT/UI queues until the interactive UI
         * has explicitly armed keypad delivery.
         */
        ESP_LOGD(TAG, "KEYPAD_EVENT_DROP reason=gate key=%ld", (long)value);
        return ESP_OK;
    }

    const int32_t payload =
        value;

    esp_err_t err =
        esp_event_post_to(
            s_loop,
            APP_EVENT,
            (int32_t)type,
            &payload,
            sizeof(payload),
            0
        );

    if (type == APP_EVENT_KEYPAD) {
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "KEYPAD_EVENT_POST key=%ld", (long)value);
        }
        else {
            ESP_LOGW(
                TAG,
                "KEYPAD_EVENT_POST_FAILED key=%ld err=%s",
                (long)value,
                esp_err_to_name(err)
            );
        }
    }

    if (log_queue_full && err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(
            TAG,
            "Event loop queue full, type=%d",
            (int)type
        );
    }

    return err;
}


esp_err_t app_events_post(
    app_event_type_t type,
    int32_t value
)
{
    return app_events_post_internal(type, value, true);
}


esp_err_t app_events_post_best_effort(
    app_event_type_t type,
    int32_t value
)
{
    return app_events_post_internal(type, value, false);
}
