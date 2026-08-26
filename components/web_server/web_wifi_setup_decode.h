#pragma once

#include <stddef.h>

#include "esp_err.h"


esp_err_t web_wifi_setup_url_decode(
    const char *source,
    char *destination,
    size_t destination_size
);
