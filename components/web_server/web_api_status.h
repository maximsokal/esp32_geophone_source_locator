#pragma once

#include "esp_err.h"
#include "esp_http_server.h"


/*
 * Register GET /api/status.
 */
esp_err_t web_api_status_register(
    httpd_handle_t server
);