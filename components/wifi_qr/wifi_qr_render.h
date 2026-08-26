#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "qrcode.h"


typedef struct
{
    int x;
    int y;
    int max_size;

    esp_err_t result;

} wifi_qr_render_context_t;


void wifi_qr_render_callback(
    esp_qrcode_handle_t qrcode,
    void *user_data
);
