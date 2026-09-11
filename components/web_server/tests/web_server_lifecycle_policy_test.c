#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "web_server_lifecycle_policy.h"

typedef struct {
    bool mdns_ready;
    bool http_running;
    bool https_running;
    bool exited;
} fake_web_resources_t;

static void assert_plan(
    web_lifecycle_state_t state,
    const web_lifecycle_action_t *expected,
    size_t expected_count
)
{
    web_lifecycle_action_t actual[WEB_LIFECYCLE_ACTIONS_MAX] = { 0 };
    assert(web_lifecycle_state_actions(
        state,
        actual,
        sizeof(actual) / sizeof(actual[0])
    ) == expected_count);

    for (size_t i = 0U; i < expected_count; ++i) {
        assert(actual[i] == expected[i]);
    }
}

static void apply_plan(
    web_lifecycle_state_t state,
    fake_web_resources_t *resources
)
{
    web_lifecycle_action_t actions[WEB_LIFECYCLE_ACTIONS_MAX] = { 0 };
    const size_t action_count = web_lifecycle_state_actions(
        state,
        actions,
        sizeof(actions) / sizeof(actions[0])
    );
    assert(action_count > 0U);

    for (size_t i = 0U; i < action_count; ++i) {
        switch (actions[i]) {
        case WEB_LIFECYCLE_ACTION_MDNS_START:
            resources->mdns_ready = true;
            break;
        case WEB_LIFECYCLE_ACTION_MDNS_STOP:
            resources->mdns_ready = false;
            break;
        case WEB_LIFECYCLE_ACTION_HTTP_START:
            resources->http_running = true;
            break;
        case WEB_LIFECYCLE_ACTION_HTTP_STOP:
            resources->http_running = false;
            break;
        case WEB_LIFECYCLE_ACTION_HTTPS_START:
            resources->https_running = true;
            break;
        case WEB_LIFECYCLE_ACTION_HTTPS_STOP:
            resources->https_running = false;
            break;
        case WEB_LIFECYCLE_ACTION_EXIT:
            resources->exited = true;
            break;
        default:
            assert(false);
            break;
        }
    }
}

int main(void)
{
    const web_lifecycle_action_t connected[] = {
        WEB_LIFECYCLE_ACTION_MDNS_START,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_START,
    };
    const web_lifecycle_action_t setup_ap[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_START,
    };
    const web_lifecycle_action_t disconnected[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
    };
    const web_lifecycle_action_t stopped[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_EXIT,
    };

    assert_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, connected, 3U);
    assert_plan(WEB_LIFECYCLE_STATE_SETUP_AP, setup_ap, 3U);
    assert_plan(WEB_LIFECYCLE_STATE_STA_DISCONNECTED, disconnected, 3U);
    assert_plan(WEB_LIFECYCLE_STATE_STOPPED, stopped, 4U);

    assert(web_lifecycle_state_actions(
        (web_lifecycle_state_t)99,
        NULL,
        0U
    ) == 0U);
    assert(web_lifecycle_state_actions(
        WEB_LIFECYCLE_STATE_STOPPED,
        NULL,
        0U
    ) == 4U);
    web_lifecycle_action_t truncated[2] = { 0 };
    assert(web_lifecycle_state_actions(
        WEB_LIFECYCLE_STATE_STOPPED,
        truncated,
        sizeof(truncated) / sizeof(truncated[0])
    ) == 4U);
    assert(truncated[0] == WEB_LIFECYCLE_ACTION_MDNS_STOP);
    assert(truncated[1] == WEB_LIFECYCLE_ACTION_HTTPS_STOP);

    fake_web_resources_t resources = { 0 };
    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    assert(resources.mdns_ready);
    assert(!resources.http_running);
    assert(resources.https_running);

    /* A pending GOT_IP is overwritten by AP_START. The final desired state
     * must still stop mDNS/HTTPS and leave the setup HTTP server running. */
    web_lifecycle_state_t queued_state = WEB_LIFECYCLE_STATE_STA_CONNECTED;
    queued_state = WEB_LIFECYCLE_STATE_SETUP_AP;
    apply_plan(queued_state, &resources);
    assert(!resources.mdns_ready);
    assert(resources.http_running);
    assert(!resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_STA_DISCONNECTED, &resources);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_STA_CONNECTED, &resources);
    assert(resources.mdns_ready);
    assert(!resources.http_running);
    assert(resources.https_running);

    apply_plan(WEB_LIFECYCLE_STATE_STOPPED, &resources);
    assert(!resources.mdns_ready);
    assert(!resources.http_running);
    assert(!resources.https_running);
    assert(resources.exited);

    puts("web lifecycle policy tests passed");
    return 0;
}
