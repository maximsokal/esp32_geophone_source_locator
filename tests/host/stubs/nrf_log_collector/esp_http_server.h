#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct httpd_req_t {
    const char *query;
    size_t query_length;
    char response[8192];
    size_t response_length;
    char status[32];
} httpd_req_t;

typedef httpd_req_t *httpd_handle_t;

typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *request);
    void *user_ctx;
} httpd_uri_t;

#define HTTP_GET 1
#define HTTPD_RESP_USE_STRLEN (-1)

size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, int length);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route);
