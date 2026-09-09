#include "web_route_capacity.h"

#include <stdbool.h>
#include <string.h>

static const web_route_identity_t s_https_routes[] = {
    { "/", WEB_ROUTE_GET },
    { "/api/client-log", WEB_ROUTE_POST },
    { "/assets/*", WEB_ROUTE_GET },
    { "/api/status", WEB_ROUTE_GET },
    { "/api/control", WEB_ROUTE_POST },
    { "/seismic", WEB_ROUTE_GET },
    { "/api/seismic/session", WEB_ROUTE_GET },
    { "/api/seismic", WEB_ROUTE_GET },
    { "/setup/verify", WEB_ROUTE_GET },
    { "/api/setup/verify", WEB_ROUTE_POST },
    { "/setup/verify-ui", WEB_ROUTE_GET },
};

/* Maximum simultaneously registered bootstrap routes. */
static const web_route_identity_t s_http_routes[] = {
    { "/", WEB_ROUTE_GET },
    { "/setup", WEB_ROUTE_GET },
    { "/setup/save", WEB_ROUTE_POST },
    { "/setup/ca", WEB_ROUTE_GET },
    { "/setup/ca-guide", WEB_ROUTE_GET },
    { "/setup/ca-download", WEB_ROUTE_GET },
};

_Static_assert(sizeof(s_https_routes) / sizeof(s_https_routes[0]) == WEB_HTTPS_URI_HANDLERS_REQUIRED,
               "HTTPS route count and configured requirement diverged");
_Static_assert(sizeof(s_http_routes) / sizeof(s_http_routes[0]) == WEB_HTTP_URI_HANDLERS_REQUIRED,
               "HTTP route count and configured requirement diverged");

const web_route_identity_t *web_https_route_catalog(size_t *count)
{
    if (count != NULL) *count = sizeof(s_https_routes) / sizeof(s_https_routes[0]);
    return s_https_routes;
}

const web_route_identity_t *web_http_route_catalog(size_t *count)
{
    if (count != NULL) *count = sizeof(s_http_routes) / sizeof(s_http_routes[0]);
    return s_http_routes;
}

bool web_route_catalog_has_duplicates(const web_route_identity_t *routes, size_t count)
{
    if (routes == NULL && count != 0U) return true;
    for (size_t i = 0U; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            if (routes[i].method == routes[j].method && strcmp(routes[i].uri, routes[j].uri) == 0) return true;
        }
    }
    return false;
}
