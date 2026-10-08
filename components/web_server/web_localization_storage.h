#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_localization_runs_send_storage_unavailable(httpd_req_t *request);
bool web_localization_runs_reject_if_storage_unavailable(
    httpd_req_t *request,
    esp_err_t *response_out
);
