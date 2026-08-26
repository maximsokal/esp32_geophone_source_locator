#pragma once

#include <stddef.h>

#include "esp_err.h"


esp_err_t web_wifi_setup_form_parse(
    char *body,
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
);
