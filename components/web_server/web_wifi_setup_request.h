#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"


esp_err_t web_wifi_setup_request_read_body(
    httpd_req_t *request,
    char *body,
    size_t body_size
);
