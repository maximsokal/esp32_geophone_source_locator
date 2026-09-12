#include "web_server_lifecycle_policy.h"


web_lifecycle_state_t web_lifecycle_effective_network_state(
    web_lifecycle_state_t event_state,
    bool radio_state_uncertain
)
{
    return radio_state_uncertain
        ? WEB_LIFECYCLE_STATE_STA_DISCONNECTED
        : event_state;
}


size_t web_lifecycle_state_actions(
    web_lifecycle_state_t state,
    web_lifecycle_action_t *actions,
    size_t action_capacity
)
{
    static const web_lifecycle_action_t sta_connected[] = {
        WEB_LIFECYCLE_ACTION_MDNS_START,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_START,
    };
    static const web_lifecycle_action_t setup_ap[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_START,
    };
    static const web_lifecycle_action_t sta_disconnected[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
    };
    static const web_lifecycle_action_t quiesced[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
    };
    static const web_lifecycle_action_t stopped[] = {
        WEB_LIFECYCLE_ACTION_MDNS_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_STOP,
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_EXIT,
    };

    const web_lifecycle_action_t *plan = NULL;
    size_t plan_count = 0U;
    switch (state) {
    case WEB_LIFECYCLE_STATE_STA_CONNECTED:
        plan = sta_connected;
        plan_count = sizeof(sta_connected) / sizeof(sta_connected[0]);
        break;

    case WEB_LIFECYCLE_STATE_SETUP_AP:
        plan = setup_ap;
        plan_count = sizeof(setup_ap) / sizeof(setup_ap[0]);
        break;

    case WEB_LIFECYCLE_STATE_STA_DISCONNECTED:
        plan = sta_disconnected;
        plan_count = sizeof(sta_disconnected) / sizeof(sta_disconnected[0]);
        break;

    case WEB_LIFECYCLE_STATE_QUIESCED:
        plan = quiesced;
        plan_count = sizeof(quiesced) / sizeof(quiesced[0]);
        break;

    case WEB_LIFECYCLE_STATE_STOPPED:
        plan = stopped;
        plan_count = sizeof(stopped) / sizeof(stopped[0]);
        break;

    default:
        return 0U;
    }

    if (actions == NULL) return plan_count;

    const size_t copy_count =
        plan_count < action_capacity ? plan_count : action_capacity;
    for (size_t i = 0U; i < copy_count; ++i) {
        actions[i] = plan[i];
    }
    return plan_count;
}
