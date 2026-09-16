#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t wifi_manager_setup_events_init(void);

void wifi_manager_setup_events_set_enabled(
    bool enabled
);

/* Retire the current setup-AP event epoch. */
void wifi_manager_setup_events_reset(void);
