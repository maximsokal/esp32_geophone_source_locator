#pragma once

#include <stdbool.h>

#include "esp_err.h"


/*
 * Explicit application health inputs. These are deliberately separate from
 * ESP_LOG severity: a warning/error becomes an indicator state only when a
 * service makes an intentional status decision.
 */
typedef enum
{
    APP_STATUS_NORMAL = 0,
    APP_STATUS_WARNING,
    APP_STATUS_ERROR,
    APP_STATUS_CLEAR_NON_STICKY

} app_status_event_t;


typedef enum
{
    STATUS_INDICATOR_NORMAL = 0, /* green */
    STATUS_INDICATOR_WARNING,    /* yellow, sticky until power cycle */
    STATUS_INDICATOR_ERROR       /* red, sticky until power cycle */

} status_indicator_state_t;


/* Subscribe to the application event loop. No task is created here. */
esp_err_t status_indicator_init(void);


/* Stop the subscription and release LEDs; state is not reset mid-boot. */
void status_indicator_stop(void);


/* Post an explicit status decision to the shared application event loop. */
esp_err_t status_indicator_post(
    app_status_event_t event
);


/* Read the effective state for diagnostics and host regression tests. */
status_indicator_state_t status_indicator_get_state(void);


/* Whether the current board has a physical LED backend configured. */
bool status_indicator_backend_enabled(void);
