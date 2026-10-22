#pragma once
#include <stddef.h>
#include "esp_err.h"
#ifdef _MSC_VER
typedef ptrdiff_t ssize_t;
#else
#include <sys/types.h>
#endif
typedef void *httpd_handle_t;
typedef struct httpd_req_t {
    size_t content_len;
    const char *body;
    size_t received;
    const char *query;
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
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_send_err(httpd_req_t *, int, const char *);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
int httpd_req_to_sockfd(httpd_req_t *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
size_t httpd_req_get_url_query_len(httpd_req_t *);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t);
esp_err_t httpd_query_key_value(const char *, const char *, char *, size_t);
