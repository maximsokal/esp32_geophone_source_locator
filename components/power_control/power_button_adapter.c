#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "sdkconfig.h"
#include "board_config.h"
#include "button_gpio.h"
#include "app_events.h"
#include "iot_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"


static const char *TAG = "POWER_BUTTON";

#define POWER_BUTTON_SHORT_PRESS_MS 50U
#define POWER_BUTTON_LONG_PRESS_MS 150U
#define POWER_BUTTON_RELEASE_CONFIRM_MS 50U
#define POWER_EVENT_SHUTDOWN_REQUESTED BIT0
#define POWER_EVENT_BUTTON_RELEASED (BIT0 << 1)

/* espressif/button converts per-device press times to scan ticks before it
 * validates BUTTON_LONG_PRESS_START registration. Keep the invariant in the
 * same units so a future timing/config change fails at build time instead of
 * causing a boot-time ESP_ERR_INVALID_ARG/reset loop. */
#define POWER_BUTTON_CONFIGURED_TICKS(time_ms) \
    ((((time_ms) / CONFIG_BUTTON_PERIOD_TIME_MS) > 0U) \
        ? ((time_ms) / CONFIG_BUTTON_PERIOD_TIME_MS) \
        : 1U)

_Static_assert(
    POWER_BUTTON_CONFIGURED_TICKS(POWER_BUTTON_LONG_PRESS_MS) >
        POWER_BUTTON_CONFIGURED_TICKS(POWER_BUTTON_SHORT_PRESS_MS),
    "Power-button long-press ticks must be greater than short-press ticks"
);


typedef void (*power_button_shutdown_notification_hook_t)(void *user_context);


typedef struct {
    EventGroupHandle_t events;
    button_handle_t button;
    power_button_shutdown_notification_hook_t notification_hook;
    void *notification_context;
    atomic_bool arm_requested;
    atomic_bool armed;
    uint32_t debounced_down_ms;
    bool debounced_down_valid;
    bool initialized;
} power_button_state_t;


static power_button_state_t s_state;


static void power_button_callback(void *button_handle, void *user_data)
{
    power_button_state_t *state = (power_button_state_t *)user_data;
    if (button_handle == NULL || state == NULL || state->events == NULL) {
        return;
    }

    const button_event_t event = iot_button_get_event(button_handle);
    if (event == BUTTON_PRESS_DOWN) {
        state->debounced_down_ms = esp_log_timestamp();
        state->debounced_down_valid = true;
        return;
    }

    if (event == BUTTON_PRESS_UP) {
        state->debounced_down_valid = false;
        xEventGroupSetBits(state->events, POWER_EVENT_BUTTON_RELEASED);

        /* A release observed during boot is deliberately not enough to arm
         * shutdown. Only the explicit post-startup arm request opens this
         * gate. If startup finishes while the button is held, this debounced
         * PRESS_UP is the event that completes arming. */
        if (atomic_load_explicit(&state->arm_requested, memory_order_acquire)) {
            const bool was_armed = atomic_exchange_explicit(
                &state->armed,
                true,
                memory_order_acq_rel
            );
            if (!was_armed) {
                ESP_LOGI(TAG, "Shutdown button armed after debounced release");
            }
        }
        return;
    }

    if (
        event != BUTTON_LONG_PRESS_START ||
        !atomic_load_explicit(&state->armed, memory_order_acquire)
    ) {
        return;
    }

    /* The bit is the authoritative safety signal. Clear a stale release bit
     * first so the shutdown waiter cannot skip the current release. Close
     * keypad admission before publishing the latch so no observer can see an
     * authoritative shutdown while new local navigation is still admitted. */
    xEventGroupClearBits(state->events, POWER_EVENT_BUTTON_RELEASED);
    app_events_set_keypad_delivery_enabled(false);
    xEventGroupSetBits(state->events, POWER_EVENT_SHUTDOWN_REQUESTED);

    const uint32_t latch_ms = esp_log_timestamp();
    const uint32_t debounced_elapsed_ms = state->debounced_down_valid
        ? latch_ms - state->debounced_down_ms
        : 0U;
    (void)debounced_elapsed_ms;
    ESP_LOGI(
        TAG,
        "POWER_BUTTON_TIMING: debounced_down_ms=%u latch_ms=%u debounced_elapsed_ms=%u configured_long_press_ms=%u",
        (unsigned)state->debounced_down_ms,
        (unsigned)latch_ms,
        (unsigned)debounced_elapsed_ms,
        (unsigned)POWER_BUTTON_LONG_PRESS_MS
    );

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
        .short_press_time = POWER_BUTTON_SHORT_PRESS_MS,
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
    atomic_init(&s_state.arm_requested, false);
    atomic_init(&s_state.armed, false);
    s_state.debounced_down_ms = 0U;
    s_state.debounced_down_valid = false;

    err = iot_button_register_cb(
        s_state.button,
        BUTTON_PRESS_DOWN,
        NULL,
        power_button_callback,
        &s_state
    );
    if (err == ESP_OK) {
        err = iot_button_register_cb(
            s_state.button,
            BUTTON_PRESS_UP,
            NULL,
            power_button_callback,
            &s_state
        );
    }
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
    ESP_LOGI(
        TAG,
        "Button ready: GPIO%d active-high pull-up/down disabled short=%ums long=%ums period=%ums shutdown=DISARMED",
        BOARD_POWER_BUTTON_GPIO,
        (unsigned)POWER_BUTTON_SHORT_PRESS_MS,
        (unsigned)POWER_BUTTON_LONG_PRESS_MS,
        (unsigned)CONFIG_BUTTON_PERIOD_TIME_MS
    );
    return ESP_OK;
}


esp_err_t power_button_adapter_arm_after_startup(void)
{
    if (!s_state.initialized || s_state.button == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(
        &s_state.arm_requested,
        true,
        memory_order_release
    );

    /* Do not arm from a transient raw LOW at the instant startup completes.
     * Verify that the official Button driver reports released on both sides
     * of one short-press/debounce interval. This is a one-time startup delay,
     * not an application polling task. */
    if (iot_button_get_key_level(s_state.button) != 0U) {
        ESP_LOGW(TAG, "Shutdown arm deferred: button is held at startup completion");
        return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_RELEASE_CONFIRM_MS));

    if (iot_button_get_key_level(s_state.button) == 0U) {
        const bool was_armed = atomic_exchange_explicit(
            &s_state.armed,
            true,
            memory_order_acq_rel
        );
        if (!was_armed) {
            ESP_LOGI(
                TAG,
                "Shutdown button armed after startup release confirmation (%ums)",
                (unsigned)POWER_BUTTON_RELEASE_CONFIRM_MS
            );
        }
    }
    else {
        ESP_LOGW(TAG, "Shutdown arm deferred: button changed to pressed during release confirmation");
    }

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

    /* The official Button delivers a debounced release event. This is not a
     * claim that the raw GPIO release-to-latch interval is exactly 50 ms. */
    xEventGroupWaitBits(
        s_state.events,
        POWER_EVENT_BUTTON_RELEASED,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(TAG, "Shutdown button latched and released (official Button debounce)");
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

    atomic_store_explicit(&s_state.arm_requested, false, memory_order_release);
    atomic_store_explicit(&s_state.armed, false, memory_order_release);

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
    s_state.debounced_down_ms = 0U;
    s_state.debounced_down_valid = false;
    s_state.initialized = false;
}
