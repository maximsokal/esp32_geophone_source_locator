/*
 * HTTP server decisions with a finite domain: network-lifecycle plans, the
 * system-status verdict truth table and the route catalog/tracker.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "web_route_capacity.h"
#include "web_server_lifecycle_policy.h"
#include "web_status_policy.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

typedef struct {
    bool mdns;
    bool http;
    bool https;
    bool exited;
} services_t;

static services_t apply_plan(services_t services, const web_lifecycle_action_t *plan, size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        switch (plan[i]) {
        case WEB_LIFECYCLE_ACTION_MDNS_START: services.mdns = true; break;
        case WEB_LIFECYCLE_ACTION_MDNS_STOP: services.mdns = false; break;
        case WEB_LIFECYCLE_ACTION_HTTP_START: services.http = true; break;
        case WEB_LIFECYCLE_ACTION_HTTP_STOP: services.http = false; break;
        case WEB_LIFECYCLE_ACTION_HTTPS_START: services.https = true; break;
        case WEB_LIFECYCLE_ACTION_HTTPS_STOP: services.https = false; break;
        case WEB_LIFECYCLE_ACTION_EXIT: services.exited = true; break;
        }
    }
    return services;
}

static void lifecycle_plans_converge_from_every_starting_point(void)
{
    /* Wanted end state per network state; -1 keeps the service as it was. */
    static const struct { int mdns; int http; int https; bool exit; } wanted[5] = {
        /* STA_CONNECTED    */ {1, 0, 1, false},   /* encrypted UI only, advertised by mDNS */
        /* SETUP_AP         */ {0, 1, 0, false},   /* plain HTTP bootstrap only */
        /* STA_DISCONNECTED */ {0, 0, 0, false},
        /* QUIESCED         */ {0, -1, 0, false},  /* during a radio transition HTTP is left alone */
        /* STOPPED          */ {0, 0, 0, true},
    };

    for (int state = WEB_LIFECYCLE_STATE_STA_CONNECTED; state <= WEB_LIFECYCLE_STATE_STOPPED; ++state) {
        web_lifecycle_action_t plan[WEB_LIFECYCLE_ACTIONS_MAX];
        const size_t count = web_lifecycle_state_actions((web_lifecycle_state_t)state, plan, WEB_LIFECYCLE_ACTIONS_MAX);
        assert(count >= 2U && count <= WEB_LIFECYCLE_ACTIONS_MAX);
        assert(web_lifecycle_state_actions((web_lifecycle_state_t)state, NULL, 0U) == count);

        /* No service is both started and stopped in one plan. */
        for (size_t i = 0U; i < count; ++i) {
            for (size_t j = i + 1U; j < count; ++j) assert(plan[i] != plan[j]);
        }
        /* EXIT is always last. */
        for (size_t i = 0U; i + 1U < count; ++i) assert(plan[i] != WEB_LIFECYCLE_ACTION_EXIT);

        for (unsigned initial = 0U; initial < 8U; ++initial) {
            const services_t start = {(initial & 1U) != 0U, (initial & 2U) != 0U, (initial & 4U) != 0U, false};
            const services_t end = apply_plan(start, plan, count);
            assert(end.mdns == (wanted[state].mdns < 0 ? start.mdns : wanted[state].mdns != 0));
            assert(end.http == (wanted[state].http < 0 ? start.http : wanted[state].http != 0));
            assert(end.https == (wanted[state].https < 0 ? start.https : wanted[state].https != 0));
            assert(end.exited == wanted[state].exit);
            /* Idempotent: repeating the plan changes nothing. */
            const services_t again = apply_plan(end, plan, count);
            assert(again.mdns == end.mdns && again.http == end.http && again.https == end.https);
        }

        /* A short caller buffer is filled with a prefix and the full length is reported. */
        for (size_t capacity = 0U; capacity <= WEB_LIFECYCLE_ACTIONS_MAX; ++capacity) {
            web_lifecycle_action_t partial[WEB_LIFECYCLE_ACTIONS_MAX + 1U];
            for (size_t i = 0U; i < sizeof(partial) / sizeof(partial[0]); ++i) partial[i] = (web_lifecycle_action_t)99;
            assert(web_lifecycle_state_actions((web_lifecycle_state_t)state, partial, capacity) == count);
            for (size_t i = 0U; i < capacity && i < count; ++i) assert(partial[i] == plan[i]);
            for (size_t i = capacity < count ? capacity : count; i < sizeof(partial) / sizeof(partial[0]); ++i) {
                assert(partial[i] == (web_lifecycle_action_t)99);
            }
        }
    }
    for (int state = WEB_LIFECYCLE_STATE_STOPPED + 1; state < 40; ++state) {
        web_lifecycle_action_t plan[WEB_LIFECYCLE_ACTIONS_MAX];
        assert(web_lifecycle_state_actions((web_lifecycle_state_t)state, plan, WEB_LIFECYCLE_ACTIONS_MAX) == 0U);
    }
    /* Uncertain radio state always falls back to the safe, fully stopped network plan. */
    for (int state = WEB_LIFECYCLE_STATE_STA_CONNECTED; state <= WEB_LIFECYCLE_STATE_STOPPED; ++state) {
        assert(web_lifecycle_effective_network_state((web_lifecycle_state_t)state, true) ==
               WEB_LIFECYCLE_STATE_STA_DISCONNECTED);
        assert(web_lifecycle_effective_network_state((web_lifecycle_state_t)state, false) ==
               (web_lifecycle_state_t)state);
    }
    assert(web_lifecycle_command_ingress_allowed(false) && !web_lifecycle_command_ingress_allowed(true));
    assert(!web_lifecycle_small_body_timeout_retry_allowed());
    CASE_PASS("HTTP.lifecycle_plans");
}

static void system_status_truth_table(void)
{
    unsigned counts[4] = {0U, 0U, 0U, 0U};

    for (unsigned flags = 0U; flags < 16U; ++flags) {
        const bool running = (flags & 1U) != 0U;
        const bool requested = (flags & 2U) != 0U;
        const bool healthy = (flags & 4U) != 0U;
        const bool error = (flags & 8U) != 0U;
        for (size_t ready = 0U; ready <= 5U; ++ready) {
            for (int pod_error = 0; pod_error <= 1; ++pod_error) {
                for (size_t required = 0U; required <= 5U; ++required) {
                    const web_system_state_t state = web_status_policy_evaluate(
                        running, requested, healthy, error, ready, pod_error != 0, required);
                    ++counts[state];
                    if (pod_error != 0 || (requested && !healthy)) {
                        /* A fault outranks everything, including a running acquisition. */
                        assert(state == WEB_SYSTEM_FAULT);
                    } else if (error) {
                        assert(state == WEB_SYSTEM_NOT_READY);
                    } else if (running && required > 0U && ready >= required) {
                        assert(state == WEB_SYSTEM_READY);
                    } else if (running || ready > 0U) {
                        assert(state == WEB_SYSTEM_DEGRADED);
                    } else {
                        assert(state == WEB_SYSTEM_NOT_READY);
                    }
                    /* READY needs a running acquisition and no error of any kind. */
                    if (state == WEB_SYSTEM_READY) {
                        assert(running && !error && pod_error == 0 && !(requested && !healthy));
                        assert(required > 0U && ready >= required);
                    }
                }
            }
        }
    }
    for (unsigned i = 0U; i < 4U; ++i) assert(counts[i] > 0U);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_READY), "READY") == 0);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_DEGRADED), "DEGRADED") == 0);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_FAULT), "FAULT") == 0);
    assert(strcmp(web_status_policy_name(WEB_SYSTEM_NOT_READY), "NOT_READY") == 0);
    assert(strcmp(web_status_policy_name((web_system_state_t)77), "NOT_READY") == 0);
    CASE_PASS("HTTP.status_verdict");
}

static void route_catalogs_and_tracker_follow_their_capacity(void)
{
    size_t https_count = 0U;
    size_t http_count = 0U;
    const web_route_identity_t *https = web_https_route_catalog(&https_count);
    const web_route_identity_t *http = web_http_route_catalog(&http_count);

    assert(https_count == WEB_HTTPS_URI_HANDLERS_REQUIRED && http_count == WEB_HTTP_URI_HANDLERS_REQUIRED);
    assert(!web_route_catalog_has_duplicates(https, https_count));
    assert(!web_route_catalog_has_duplicates(http, http_count));
    assert(https_count <= WEB_HTTPS_URI_HANDLERS_CAPACITY && http_count <= WEB_HTTP_URI_HANDLERS_CAPACITY);
    assert(web_route_catalog_has_duplicates(NULL, 1U));
    /* Control commands never exist on the unencrypted listener; Wi-Fi setup never on the encrypted one. */
    for (size_t i = 0U; i < http_count; ++i) assert(strcmp(http[i].uri, "/api/control") != 0);
    for (size_t i = 0U; i < https_count; ++i) assert(strcmp(https[i].uri, "/api/wifi/setup") != 0);
    for (size_t i = 0U; i < https_count; ++i) {
        if (strcmp(https[i].uri, "/api/control") == 0) assert(https[i].method == WEB_ROUTE_POST);
    }

    for (size_t capacity = 0U; capacity <= WEB_ROUTE_TRACKER_MAX_HANDLERS + 2U; ++capacity) {
        web_route_tracker_t tracker;
        const bool ok = web_route_tracker_init(&tracker, capacity);
        assert(ok == (capacity >= 1U && capacity <= WEB_ROUTE_TRACKER_MAX_HANDLERS));
    }
    web_route_tracker_t tracker;
    assert(web_route_tracker_init(&tracker, WEB_HTTPS_URI_HANDLERS_CAPACITY));
    for (size_t i = 0U; i < https_count; ++i) assert(web_route_tracker_register(&tracker, https[i]));
    assert(web_route_tracker_matches_catalog(&tracker, https, https_count));
    assert(!web_route_tracker_register(&tracker, https[0])); /* duplicate */
    assert(!web_route_tracker_matches_catalog(&tracker, http, http_count));
    assert(web_route_tracker_register(&tracker, (web_route_identity_t){"/extra", WEB_ROUTE_GET})); /* the reserve slot */
    assert(!web_route_tracker_register(&tracker, (web_route_identity_t){"/over", WEB_ROUTE_GET})); /* full */
    assert(!web_route_tracker_matches_catalog(&tracker, https, https_count));
    assert(web_route_tracker_unregister(&tracker, (web_route_identity_t){"/extra", WEB_ROUTE_GET}));
    assert(!web_route_tracker_unregister(&tracker, (web_route_identity_t){"/extra", WEB_ROUTE_GET}));
    assert(web_route_tracker_matches_catalog(&tracker, https, https_count));
    /* The same URI under another method is a different route. */
    assert(web_route_tracker_unregister(&tracker, https[0]));
    assert(!web_route_tracker_unregister(&tracker, (web_route_identity_t){https[0].uri, https[0].method == WEB_ROUTE_GET ? WEB_ROUTE_POST : WEB_ROUTE_GET}));
    assert(!web_route_tracker_register(NULL, https[0]));
    assert(!web_route_tracker_register(&tracker, (web_route_identity_t){NULL, WEB_ROUTE_GET}));
    CASE_PASS("HTTP.route_catalog");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    lifecycle_plans_converge_from_every_starting_point();
    system_status_truth_table();
    route_catalogs_and_tracker_follow_their_capacity();
    return 0;
}
