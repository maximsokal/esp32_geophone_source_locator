#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t wifi_manager_sta_events_init(void);

void wifi_manager_sta_events_set_connect_enabled(
    bool enabled
);

/* Retire the current STA event epoch and clear accepted runtime IP state. */
void wifi_manager_sta_events_reset(void);
