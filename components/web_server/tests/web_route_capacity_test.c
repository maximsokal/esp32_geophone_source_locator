#include "web_route_capacity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    size_t count = 0U;
    const web_route_identity_t *routes = web_https_route_catalog(&count);
    assert(count == WEB_HTTPS_URI_HANDLERS_REQUIRED);
    assert(count <= WEB_HTTPS_URI_HANDLERS_CAPACITY);
    assert(WEB_HTTPS_URI_HANDLERS_CAPACITY == 15U);
    assert(WEB_HTTPS_URI_HANDLER_RESERVE == 1U);
    assert(!web_route_catalog_has_duplicates(routes, count));
    bool has_nrf_logs = false;
    for (size_t i = 0U; i < count; ++i) {
        if (routes[i].method == WEB_ROUTE_GET && strcmp(routes[i].uri, "/api/nrf/logs") == 0) {
            has_nrf_logs = true;
        }
    }
    assert(has_nrf_logs);

    /* The phone sends the result it calculated with POST to the runs URI; the download modes share its GET. */
    bool has_runs_get = false, has_runs_post = false;
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(routes[i].uri, "/api/localization/runs") != 0) continue;
        has_runs_get = has_runs_get || routes[i].method == WEB_ROUTE_GET;
        has_runs_post = has_runs_post || routes[i].method == WEB_ROUTE_POST;
    }
    assert(has_runs_get && has_runs_post);

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
