#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef struct {
    int content_len;
    const char *body;
    size_t offset;
    size_t chunk_size;
    int recv_override;
} httpd_req_t;

typedef enum {
    HTTP_GET = 0,
    HTTP_POST = 1,
} httpd_method_t;

typedef esp_err_t (*httpd_uri_func_t)(httpd_req_t *request);

typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_func_t handler;
    void *user_ctx;
} httpd_uri_t;

#define HTTPD_RESP_USE_STRLEN (-1)

esp_err_t httpd_register_uri_handler(
    httpd_handle_t server,
    const httpd_uri_t *uri
);

int httpd_req_recv(
    httpd_req_t *request,
    char *buffer,
    size_t length
);
