#pragma once

#include <stdbool.h>
#include <stddef.h>

#define WEB_HTTPS_URI_HANDLERS_REQUIRED  14U
#define WEB_HTTP_URI_HANDLERS_REQUIRED    6U
#define WEB_URI_HANDLER_RESERVE           2U
#define WEB_HTTPS_URI_HANDLER_RESERVE     1U
#define WEB_HTTPS_URI_HANDLERS_CAPACITY  (WEB_HTTPS_URI_HANDLERS_REQUIRED + WEB_HTTPS_URI_HANDLER_RESERVE)
#define WEB_HTTP_URI_HANDLERS_CAPACITY   (WEB_HTTP_URI_HANDLERS_REQUIRED + WEB_URI_HANDLER_RESERVE)
#define WEB_ROUTE_TRACKER_MAX_HANDLERS    16U

typedef enum {
    WEB_ROUTE_GET = 0,
    WEB_ROUTE_POST,
} web_route_method_t;

typedef struct {
    const char *uri;
    web_route_method_t method;
} web_route_identity_t;

typedef struct {
    web_route_identity_t active[WEB_ROUTE_TRACKER_MAX_HANDLERS];
    size_t count;
    size_t capacity;
} web_route_tracker_t;

const web_route_identity_t *web_https_route_catalog(size_t *count);
const web_route_identity_t *web_http_route_catalog(size_t *count);
bool web_route_catalog_has_duplicates(const web_route_identity_t *routes, size_t count);
bool web_route_tracker_init(web_route_tracker_t *tracker, size_t capacity);
bool web_route_tracker_register(web_route_tracker_t *tracker, web_route_identity_t route);
bool web_route_tracker_unregister(web_route_tracker_t *tracker, web_route_identity_t route);
bool web_route_tracker_matches_catalog(
    const web_route_tracker_t *tracker,
    const web_route_identity_t *catalog,
    size_t catalog_count
);
