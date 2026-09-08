#pragma once

#include <stddef.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef _MSC_VER
typedef long ssize_t;
#endif

typedef void *httpd_handle_t;

typedef struct httpd_req_t {
    const char *uri;
} httpd_req_t;

typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *request);
    void *user_ctx;
} httpd_uri_t;

#define HTTP_GET              1
#define HTTPD_RESP_USE_STRLEN (-1)
#define HTTPD_404_NOT_FOUND   404

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length);
esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length);
esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri);
