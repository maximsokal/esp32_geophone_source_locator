#pragma once

#include <stdbool.h>

#include "esp_err.h"


/*
 * Initialize the optional ESP Insights bridge. The baseline configuration is
 * deliberately a no-op and never needs an auth key or network access.
 */
esp_err_t insights_bridge_init(void);


/* Publish only selected runtime health counters when the bridge is enabled. */
esp_err_t insights_bridge_publish_runtime(void);


bool insights_bridge_enabled(void);
