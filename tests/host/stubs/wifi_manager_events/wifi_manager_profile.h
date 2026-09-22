#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t wifi_manager_profile_is_known_client(
    const uint8_t mac[6],
    bool *known
);
