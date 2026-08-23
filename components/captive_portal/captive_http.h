#pragma once

#include "esp_err.h"
#include "esp_http_server.h"


esp_err_t captive_http_not_found_handler(
    httpd_req_t *request,
    httpd_err_code_t error
);