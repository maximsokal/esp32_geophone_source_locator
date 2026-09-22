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
    const char *query;
    const char *body;
    int content_len;
    size_t body_offset;
    char *response_body;
    size_t response_capacity;
    size_t response_length;
    const char *content_type;
    const char *cache_control;
    const char *response_status;
    const char *scan_bytes_header;
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
#define HTTPD_SOCK_ERR_TIMEOUT (-2)
#define HTTPD_400_BAD_REQUEST 400

size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size);
esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t value_size);
int httpd_req_recv(httpd_req_t *request, void *buffer, size_t length);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri);
