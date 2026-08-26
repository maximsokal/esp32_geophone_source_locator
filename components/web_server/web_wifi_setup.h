#pragma once

#include "esp_err.h"
#include "esp_http_server.h"


esp_err_t web_wifi_setup_send_page(
    httpd_req_t *request
);

esp_err_t web_wifi_setup_register(
    httpd_handle_t server
);
