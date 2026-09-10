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

    web_route_tracker_t tracker;
    assert(web_route_tracker_init(&tracker, 2U));
    assert(web_route_tracker_register(&tracker, (web_route_identity_t){ "/a", WEB_ROUTE_GET }));
    assert(!web_route_tracker_register(&tracker, (web_route_identity_t){ "/a", WEB_ROUTE_GET }));
    assert(web_route_tracker_register(&tracker, (web_route_identity_t){ "/b", WEB_ROUTE_POST }));
    assert(!web_route_tracker_register(&tracker, (web_route_identity_t){ "/c", WEB_ROUTE_GET }));
    assert(web_route_tracker_unregister(&tracker, (web_route_identity_t){ "/a", WEB_ROUTE_GET }));
    assert(!web_route_tracker_unregister(&tracker, (web_route_identity_t){ "/missing", WEB_ROUTE_GET }));
    assert(web_route_tracker_register(&tracker, (web_route_identity_t){ "/c", WEB_ROUTE_GET }));
    const web_route_identity_t expected[] = {
        { "/b", WEB_ROUTE_POST }, { "/c", WEB_ROUTE_GET }
    };
    assert(web_route_tracker_matches_catalog(&tracker, expected, 2U));
    const web_route_identity_t stale[] = {
        { "/b", WEB_ROUTE_POST }, { "/old", WEB_ROUTE_GET }
    };
    assert(!web_route_tracker_matches_catalog(&tracker, stale, 2U));
    puts("web route capacity tests passed");
    return 0;
}
