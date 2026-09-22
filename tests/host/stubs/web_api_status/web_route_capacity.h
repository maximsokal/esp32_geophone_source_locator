#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    const char *uri;
    int method;
} web_route_identity_t;

esp_err_t web_server_route_tracking_register(
    const char *uri,
    int method
);

static inline void web_server_route_tracking_begin(size_t capacity)
{
    (void)capacity;
}

static inline esp_err_t web_server_route_tracking_finish(
    const web_route_identity_t *catalog,
    size_t count
)
{
    (void)catalog;
    (void)count;
    return ESP_OK;
}
