#pragma once

#include <stdbool.h>

#include "esp_err.h"


/*
 * Initialize the optional ESP Insights bridge. The baseline configuration is
 * deliberately a no-op and never needs an auth key or network access.
 */
esp_err_t insights_bridge_init(void);


/* Notify the opt-in retry worker after the application event loop has observed
 * APP_EVENT_WIFI_STA_CONNECTED (the station got an IP). This function only
 * signals the worker; esp_insights_init() never runs in an event callback. */
void insights_bridge_on_network_ready(void);


/* Drain bridge-owned event/task/transport lifetime before Wi-Fi stops. */
void insights_bridge_shutdown(void);


/* Publish only selected runtime health counters when the bridge is enabled. */
esp_err_t insights_bridge_publish_runtime(void);


bool insights_bridge_enabled(void);
