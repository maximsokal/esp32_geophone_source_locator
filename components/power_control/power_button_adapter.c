#include <stdbool.h>

#include "board_config.h"
#include "button_gpio.h"
#include "app_events.h"
#include "iot_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"


static const char *TAG = "POWER_BUTTON";

#define POWER_BUTTON_LONG_PRESS_MS 150U
#define POWER_BUTTON_RELEASE_CONFIRM_MS 50U
#define POWER_EVENT_SHUTDOWN_REQUESTED BIT0
#define POWER_EVENT_BUTTON_RELEASED (BIT0 << 1)


typedef void (*power_button_shutdown_notification_hook_t)(void *user_context);


typedef struct {
    EventGroupHandle_t events;
    button_handle_t button;
    power_button_shutdown_notification_hook_t notification_hook;
    void *notification_context;
    volatile bool armed;
    bool initialized;
} power_button_state_t;


static power_button_state_t s_state;


static void power_button_callback(void *button_handle, void *user_data)
{
    (void)button_handle;

    power_button_state_t *state = (power_button_state_t *)user_data;
    if (state == NULL || state->events == NULL) {
        return;
    }

    const button_event_t event = iot_button_get_event(button_handle);
    if (event == BUTTON_PRESS_UP) {
        /* This also arms a button that was held during startup. */
        state->armed = true;
        xEventGroupSetBits(state->events, POWER_EVENT_BUTTON_RELEASED);
        return;
    }

    if (event != BUTTON_LONG_PRESS_START || !state->armed) {
        return;
    }

    /* The bit is the authoritative safety signal. Clear a stale release bit
     * first so the shutdown waiter cannot skip the current release. */
    xEventGroupClearBits(state->events, POWER_EVENT_BUTTON_RELEASED);
    xEventGroupSetBits(state->events, POWER_EVENT_SHUTDOWN_REQUESTED);

    /* The APP_LOOP notification is also zero-wait and intentionally has no
     * queue-full logging in this critical callback. */
    (void)app_events_post_best_effort(
        APP_EVENT_POWER_SHUTDOWN_REQUESTED,
        0
    );

    /* Optional notification is deliberately after the EventGroup latch and
     * APP_LOOP post; it is never required for safe shutdown. */
    if (state->notification_hook != NULL) {
        state->notification_hook(state->notification_context);
    }
}


esp_err_t power_button_adapter_init(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    s_state.events = xEventGroupCreate();
    if (s_state.events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(
        s_state.events,
        POWER_EVENT_SHUTDOWN_REQUESTED | POWER_EVENT_BUTTON_RELEASED
    );

    const button_config_t button_config = {
        .short_press_time = POWER_BUTTON_LONG_PRESS_MS,
        .long_press_time = POWER_BUTTON_LONG_PRESS_MS,
    };
    const button_gpio_config_t gpio_config = {
        .gpio_num = BOARD_POWER_BUTTON_GPIO,
        .active_level = 1U,
        .enable_power_save = false,
        .disable_pull = true,
    };

    esp_err_t err = iot_button_new_gpio_device(
        &button_config,
        &gpio_config,
        &s_state.button
    );
    if (err != ESP_OK) {
        vEventGroupDelete(s_state.events);
        s_state.events = NULL;
        return err;
    }

    s_state.notification_hook = hook;
    s_state.notification_context = user_context;
    /* Existing behavior waits for a startup-held button to be released before
     * arming. GPIO21 is already configured by Button with pulls disabled. */
    s_state.armed = gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 0;

    err = iot_button_register_cb(
        s_state.button,
        BUTTON_PRESS_UP,
        NULL,
        power_button_callback,
        &s_state
    );
    if (err == ESP_OK) {
        err = iot_button_register_cb(
            s_state.button,
            BUTTON_LONG_PRESS_START,
            NULL,
            power_button_callback,
            &s_state
        );
    }
    if (err != ESP_OK) {
        (void)iot_button_delete(s_state.button);
        s_state.button = NULL;
        vEventGroupDelete(s_state.events);
        s_state.events = NULL;
        return err;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "Button ready: GPIO%d active-high pull-up/down disabled",
        BOARD_POWER_BUTTON_GPIO);
    return ESP_OK;
}


esp_err_t power_button_adapter_set_notification_hook(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
)
{
    s_state.notification_hook = hook;
    s_state.notification_context = user_context;
    return ESP_OK;
}


void power_button_adapter_wait_for_shutdown_request(void)
{
    if (!s_state.initialized || s_state.events == NULL) {
        return;
    }

    xEventGroupWaitBits(
        s_state.events,
        POWER_EVENT_SHUTDOWN_REQUESTED,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    /* Preserve the old release-confirmation policy without a polling task. */
    xEventGroupWaitBits(
        s_state.events,
        POWER_EVENT_BUTTON_RELEASED,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(TAG, "Shutdown button latched and released (%u ms policy)",
        POWER_BUTTON_RELEASE_CONFIRM_MS);
}


bool power_button_adapter_is_shutdown_requested(void)
{
    if (!s_state.initialized || s_state.events == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_state.events) &
        POWER_EVENT_SHUTDOWN_REQUESTED) != 0;
}


void power_button_adapter_stop(void)
{
    if (!s_state.initialized) {
        return;
    }

    if (s_state.button != NULL) {
        (void)iot_button_delete(s_state.button);
        s_state.button = NULL;
    }
    if (s_state.events != NULL) {
        vEventGroupDelete(s_state.events);
        s_state.events = NULL;
    }

    s_state.notification_hook = NULL;
    s_state.notification_context = NULL;
    s_state.armed = false;
    s_state.initialized = false;
}
