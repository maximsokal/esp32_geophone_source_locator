#pragma once

#include "esp_err.h"


/*
 * Generate and draw a QR code containing the fixed
 * Wi-Fi credentials from app_config.
 */
esp_err_t wifi_qr_draw(
    int x,
    int y,
    int max_size
);