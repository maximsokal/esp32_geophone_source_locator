#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "web_server_lifecycle_policy.h"


static bool fake_running;
static bool fake_setup_mode;
static bool fake_uncertain;
static bool fake_events_suppressed;
static bool fake_sta_has_ip;


bool wifi_manager_is_running(void)
{
    return fake_running;
}


bool wifi_manager_is_setup_mode(void)
{
    return fake_setup_mode;
}


bool wifi_manager_is_radio_state_uncertain(void)
{
    return fake_uncertain;
}


bool wifi_manager_network_events_suppressed(void)
{
    return fake_events_suppressed;
}


bool wifi_manager_sta_has_ip(void)
{
    return fake_sta_has_ip;
}


static void set_authority(
    bool running,
    bool setup_mode,
    bool uncertain,
    bool events_suppressed,
    bool sta_has_ip
)
{
    fake_running = running;
    fake_setup_mode = setup_mode;
    fake_uncertain = uncertain;
    fake_events_suppressed = events_suppressed;
    fake_sta_has_ip = sta_has_ip;
}


int main(void)
{
    set_authority(false, false, false, false, false);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_STA_CONNECTED,
        false
    ) == WEB_LIFECYCLE_STATE_STA_DISCONNECTED);

    /* A retired STA GOT_IP cannot replace authoritative setup mode. */
    set_authority(true, true, false, false, false);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_STA_CONNECTED,
        false
    ) == WEB_LIFECYCLE_STATE_SETUP_AP);

    /* A retired AP_START/disconnect cannot replace a newer live STA. */
    set_authority(true, false, false, false, true);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_SETUP_AP,
        false
    ) == WEB_LIFECYCLE_STATE_STA_CONNECTED);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_STA_DISCONNECTED,
        false
    ) == WEB_LIFECYCLE_STATE_STA_CONNECTED);

    set_authority(true, false, false, false, false);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_STA_CONNECTED,
        false
    ) == WEB_LIFECYCLE_STATE_STA_DISCONNECTED);

    /* Raw callbacks cannot select the target mode before final commit. */
    set_authority(true, false, false, true, true);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_SETUP_AP,
        false
    ) == WEB_LIFECYCLE_STATE_QUIESCED);

    /* Once final state is committed, transaction serialization may still be
     * unwinding but callbacks resolve to that final state, not QUIESCED. */
    set_authority(true, true, false, false, false);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_STA_CONNECTED,
        false
    ) == WEB_LIFECYCLE_STATE_SETUP_AP);

    set_authority(true, true, true, false, false);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_SETUP_AP,
        false
    ) == WEB_LIFECYCLE_STATE_STA_DISCONNECTED);

    set_authority(true, false, false, false, true);
    assert(web_lifecycle_effective_network_state(
        WEB_LIFECYCLE_STATE_STA_CONNECTED,
        true
    ) == WEB_LIFECYCLE_STATE_STA_DISCONNECTED);

    puts("WEB_NETWORK_AUTHORITY_POLICY_HOST_PASS");
    return 0;
}
