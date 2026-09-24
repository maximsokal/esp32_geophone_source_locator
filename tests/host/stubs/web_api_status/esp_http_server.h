#pragma once

/* MSVC has no ssize_t; <sys/types.h> below provides it elsewhere. */
#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#include <stddef.h>
#include <sys/types.h>

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef struct {
    const char *query;
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

#define HTTPD_RESP_USE_STRLEN              (-1)
#define HTTPD_500_INTERNAL_SERVER_ERROR    500

esp_err_t httpd_register_uri_handler(
    httpd_handle_t server,
    const httpd_uri_t *uri
);

size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(
    httpd_req_t *request,
    char *buffer,
    size_t buffer_size
);
esp_err_t httpd_query_key_value(
    const char *query,
    const char *key,
    char *value,
    size_t value_size
);

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(
    httpd_req_t *request,
    const char *field,
    const char *value
);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_send(
    httpd_req_t *request,
    const char *buffer,
    ssize_t length
);
esp_err_t httpd_resp_send_err(
    httpd_req_t *request,
    int status,
    const char *message
);
