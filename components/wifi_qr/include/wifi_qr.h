#pragma once

#include "esp_err.h"


esp_err_t wifi_qr_draw(
    int x,
    int y,
    int max_size
);

esp_err_t wifi_qr_draw_url(
    const char *url,
    int x,
    int y,
    int max_size
);
