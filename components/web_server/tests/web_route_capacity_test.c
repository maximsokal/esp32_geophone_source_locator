#include "web_route_capacity.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    size_t count = 0U;
    const web_route_identity_t *routes = web_https_route_catalog(&count);
    assert(count == WEB_HTTPS_URI_HANDLERS_REQUIRED);
    assert(count <= WEB_HTTPS_URI_HANDLERS_CAPACITY);
    assert(!web_route_catalog_has_duplicates(routes, count));

    routes = web_http_route_catalog(&count);
    assert(count == WEB_HTTP_URI_HANDLERS_REQUIRED);
    assert(count <= WEB_HTTP_URI_HANDLERS_CAPACITY);
    assert(!web_route_catalog_has_duplicates(routes, count));

    const web_route_identity_t duplicate[] = {
        { "/same", WEB_ROUTE_GET }, { "/same", WEB_ROUTE_GET }
    };
    assert(web_route_catalog_has_duplicates(duplicate, 2U));
    puts("web route capacity tests passed");
    return 0;
}
