#pragma once

#include <stdbool.h>

#include "esp_err.h"


/*
 * Start DNS redirection and HTTP captive portal handling.
 */
esp_err_t captive_portal_start(void);


/*
 * Stop captive portal services.
 */
esp_err_t captive_portal_stop(void);


/*
 * Return true while the captive portal is active.
 */
bool captive_portal_is_running(void);