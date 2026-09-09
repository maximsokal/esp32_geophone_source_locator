#pragma once

#include <stdbool.h>
#include <stddef.h>

#define WEB_HTTPS_URI_HANDLERS_REQUIRED  11U
#define WEB_HTTP_URI_HANDLERS_REQUIRED    6U
#define WEB_URI_HANDLER_RESERVE           2U
#define WEB_HTTPS_URI_HANDLERS_CAPACITY  (WEB_HTTPS_URI_HANDLERS_REQUIRED + WEB_URI_HANDLER_RESERVE)
#define WEB_HTTP_URI_HANDLERS_CAPACITY   (WEB_HTTP_URI_HANDLERS_REQUIRED + WEB_URI_HANDLER_RESERVE)

typedef enum {
    WEB_ROUTE_GET = 0,
    WEB_ROUTE_POST,
} web_route_method_t;

typedef struct {
    const char *uri;
    web_route_method_t method;
} web_route_identity_t;

const web_route_identity_t *web_https_route_catalog(size_t *count);
const web_route_identity_t *web_http_route_catalog(size_t *count);
bool web_route_catalog_has_duplicates(const web_route_identity_t *routes, size_t count);
