#pragma once

#include <stddef.h>
#ifndef _MSC_VER
#include <sys/types.h>
#endif
#include "esp_err.h"

/* The native Windows C runtime already declares ssize_t for Clang/MinGW.
 * MSVC does not, so provide the narrow declaration needed by this fake. */
#ifdef _MSC_VER
typedef ptrdiff_t ssize_t;
#endif

typedef void *httpd_handle_t;
typedef struct httpd_req_t {
    const char *uri;
    const char *query;
    char *response_body;
    size_t response_capacity;
    size_t response_length;
    const char *content_type;
    const char *cache_control;
    unsigned error_status;
} httpd_req_t;

typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *request);
    void *user_ctx;
} httpd_uri_t;

#define HTTP_GET 1
#define HTTP_POST 2
#define HTTPD_RESP_USE_STRLEN (-1)
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_500_INTERNAL_SERVER_ERROR 500

size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size);
esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t value_size);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length);
esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length);
esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri);