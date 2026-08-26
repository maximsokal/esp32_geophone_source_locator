#pragma once

#include "esp_err.h"
#include "esp_http_server.h"


esp_err_t web_wifi_setup_page_send_form(
    httpd_req_t *request
);

esp_err_t web_wifi_setup_page_send_saved(
    httpd_req_t *request
);

esp_err_t web_wifi_setup_page_open_handler(
    httpd_req_t *request
);
