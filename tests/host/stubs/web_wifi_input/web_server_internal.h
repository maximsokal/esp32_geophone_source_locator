#pragma once

#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"

static inline size_t strlcpy(char *destination, const char *source, size_t size)
{
    const size_t source_length = strlen(source);
    if (size != 0U) {
        const size_t copy_length = source_length < size - 1U
            ? source_length
            : size - 1U;
        memcpy(destination, source, copy_length);
        destination[copy_length] = '\0';
    }
    return source_length;
}

/* Mirrors components/web_server/web_server_internal.h. */
#define WEB_WIFI_SETUP_BODY_MAX 320U

esp_err_t web_wifi_setup_url_decode(
    const char *source,
    char *destination,
    size_t destination_size
);

esp_err_t web_wifi_setup_form_parse(
    char *body,
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
);

esp_err_t web_wifi_setup_request_read_body(
    httpd_req_t *request,
    char *body,
    size_t body_size
);
