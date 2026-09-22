#pragma once

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef struct httpd_req_t {
    int unused;
} httpd_req_t;

typedef struct {
    const char *uri;
    int method;
    void *handler;
    void *user_ctx;
} httpd_uri_t;

esp_err_t httpd_register_uri_handler(
    httpd_handle_t server,
    const httpd_uri_t *uri_handler
);
