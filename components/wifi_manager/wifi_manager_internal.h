#pragma once

#include <stdbool.h>

#include "esp_err.h"


bool wifi_manager_internal_initialized(void);

void wifi_manager_internal_set_running(
    bool running
);

void wifi_manager_internal_set_setup_mode(
    bool setup_mode
);

bool wifi_manager_internal_setup_mode(void);

esp_err_t wifi_manager_internal_start_setup_mode(void);
