#include "status_indicator.h"

#include <stdint.h>

#include "app_events.h"
#include "device_status.h"

#include "esp_event.h"
#include "esp_log.h"
#include "led_indicator.h"


static const char *TAG = "STATUS_INDICATOR";


static bool s_registered = false;
static bool s_warning_sticky = false;
static bool s_error_sticky = false;
static status_indicator_state_t s_state = STATUS_INDICATOR_NORMAL;


static status_indicator_state_t status_indicator_effective_state(void)
{
    if (s_error_sticky) {
        return STATUS_INDICATOR_ERROR;
    }

    if (s_warning_sticky) {
        return STATUS_INDICATOR_WARNING;
    }

    return STATUS_INDICATOR_NORMAL;
}


static void status_indicator_apply(app_status_event_t event)
{
    switch (event) {
        case APP_STATUS_WARNING:
            s_warning_sticky = true;
            break;

        case APP_STATUS_ERROR:
            s_error_sticky = true;
            break;

        case APP_STATUS_CLEAR_NON_STICKY:
        case APP_STATUS_NORMAL:
        default:
            /* Sticky states survive until the next power cycle. */
            break;
    }

    s_state = status_indicator_effective_state();
}


static void status_indicator_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;
    (void)event_data;

    switch ((app_event_type_t)event_id) {
        case APP_EVENT_STATUS_WARNING:
            status_indicator_apply(APP_STATUS_WARNING);
            break;

        case APP_EVENT_STATUS_ERROR:
            status_indicator_apply(APP_STATUS_ERROR);
            break;

        case APP_EVENT_STATUS_CLEAR_NON_STICKY:
            status_indicator_apply(APP_STATUS_CLEAR_NON_STICKY);
            break;

        default:
            return;
    }

    ESP_LOGI(TAG, "status=%d warning_sticky=%d error_sticky=%d",
        (int)s_state,
        s_warning_sticky ? 1 : 0,
        s_error_sticky ? 1 : 0);
}


esp_err_t status_indicator_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    esp_event_loop_handle_t loop = app_events_get_loop();
    if (loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_event_handler_register_with(
        loop,
        APP_EVENT,
        ESP_EVENT_ANY_ID,
        status_indicator_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        return err;
    }

    s_state = status_indicator_effective_state();
    s_registered = true;

    /*
     * Keep the official led_indicator dependency at this boundary, but do
     * not construct a GPIO backend until the board wiring ledger names all
     * three physical LEDs. The current board has no such GPIO contract.
     */
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    ESP_LOGW(TAG, "LED backend requested but no board LED GPIO contract exists");
#else
    ESP_LOGI(TAG, "LED backend disabled: board has no connected status LED GPIOs");
#endif

    return ESP_OK;
}


void status_indicator_stop(void)
{
    if (!s_registered) {
        return;
    }

    esp_event_loop_handle_t loop = app_events_get_loop();
    if (loop != NULL) {
        (void)esp_event_handler_unregister_with(
            loop,
            APP_EVENT,
            ESP_EVENT_ANY_ID,
            status_indicator_event_handler
        );
    }

    s_registered = false;
}


esp_err_t status_indicator_post(app_status_event_t event)
{
    app_event_type_t app_event;

    switch (event) {
        case APP_STATUS_WARNING:
            app_event = APP_EVENT_STATUS_WARNING;
            break;
        case APP_STATUS_ERROR:
            app_event = APP_EVENT_STATUS_ERROR;
            break;
        case APP_STATUS_CLEAR_NON_STICKY:
            app_event = APP_EVENT_STATUS_CLEAR_NON_STICKY;
            break;
        case APP_STATUS_NORMAL:
            /* Normal is an explicit non-sticky input; it never clears latches. */
            app_event = APP_EVENT_STATUS_CLEAR_NON_STICKY;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return app_events_post(app_event, 0);
}


status_indicator_state_t status_indicator_get_state(void)
{
    return s_state;
}


bool status_indicator_backend_enabled(void)
{
    /* No GPIO is invented by enabling the future-facing Kconfig switch. */
    return false;
}
