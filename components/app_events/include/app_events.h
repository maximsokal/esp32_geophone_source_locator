#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"


#define APP_TLS_VERIFY_SECURE_CONTEXT  (1U << 0)
#define APP_TLS_VERIFY_CAMERA          (1U << 1)
#define APP_TLS_VERIFY_XR_API          (1U << 2)
#define APP_TLS_VERIFY_IMMERSIVE_AR    (1U << 3)

#define APP_TLS_VERIFY_REQUIRED_MASK   \
    (APP_TLS_VERIFY_SECURE_CONTEXT |   \
     APP_TLS_VERIFY_CAMERA |           \
     APP_TLS_VERIFY_XR_API |           \
     APP_TLS_VERIFY_IMMERSIVE_AR)


ESP_EVENT_DECLARE_BASE(APP_EVENT);


typedef enum
{
    APP_EVENT_KEYPAD = 0,

    APP_EVENT_WIFI_SETUP_CLIENT_READY,
    APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED,
    APP_EVENT_WIFI_SETUP_PAGE_OPENED,
    APP_EVENT_WIFI_CREDENTIALS_SAVED,
    APP_EVENT_WIFI_STA_CONNECTING,
    APP_EVENT_WIFI_STA_CONNECTED,
    APP_EVENT_WIFI_STA_DISCONNECTED,

    APP_EVENT_TLS_CA_DOWNLOADED,
    APP_EVENT_TLS_CA_INSTALL_CONFIRMED,
    APP_EVENT_TLS_VERIFY_READY,
    APP_EVENT_TLS_VERIFY_RESULT,

    /* Additive status/power notifications; existing IDs remain unchanged. */
    APP_EVENT_STATUS_WARNING,
    APP_EVENT_STATUS_ERROR,
    APP_EVENT_STATUS_CLEAR_NON_STICKY,
    APP_EVENT_POWER_SHUTDOWN_REQUESTED,

    /* Background archive maintenance is UI-visible but non-blocking. */
    APP_EVENT_STORAGE_RECOVERY_STARTED,
    APP_EVENT_STORAGE_RECOVERY_FINISHED,

} app_event_type_t;


esp_err_t app_events_init(void);

esp_event_loop_handle_t app_events_get_loop(void);

/*
 * Keypad Button objects start polling before the interactive UI exists.
 * Delivery stays disabled until startup explicitly arms the keypad after
 * ui_init(), so boot-time presses can never be replayed into a later screen.
 */
void app_events_set_keypad_delivery_enabled(bool enabled);

/*
 * Acquire the current keypad-admission gate. Consumers use this when work may
 * already be queued locally: closing delivery must also prevent that queued
 * navigation from executing after an authoritative shutdown request.
 */
bool app_events_keypad_delivery_enabled(void);

esp_err_t app_events_post(
    app_event_type_t type,
    int32_t value
);

/* Same zero-wait post, without diagnostic logging on queue-full. This is for
 * callbacks that must keep their safety path independent of the APP_LOOP
 * queue and must return immediately. */
esp_err_t app_events_post_best_effort(
    app_event_type_t type,
    int32_t value
);
