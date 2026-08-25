#pragma once

#include "esp_err.h"

#include "esp_http_server.h"


esp_err_t web_page_send_index(
    httpd_req_t *request
);
