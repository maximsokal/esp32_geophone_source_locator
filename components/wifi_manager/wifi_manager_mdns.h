#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t wifi_manager_mdns_init(void);

void wifi_manager_mdns_stop(void);

bool wifi_manager_mdns_is_ready(void);
