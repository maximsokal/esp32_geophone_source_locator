#include "web_server_lifecycle_policy.h"


/*
 * Firmware builds resolve Wi-Fi authority through weak GCC references so this
 * policy remains host-testable without linking the Wi-Fi component. MSVC has
 * no compatible weak-function reference. A dedicated host integration target
 * can define WEB_LIFECYCLE_LINKED_WIFI_AUTHORITY to link explicit fake/current
 * authority functions and exercise the same authoritative branch. Ordinary
 * isolated MSVC tests keep the legacy event-state fallback and emit no Wi-Fi
 * linker references.
 */
#if defined(WEB_LIFECYCLE_LINKED_WIFI_AUTHORITY)
#define WEB_LIFECYCLE_HAS_WIFI_AUTHORITY 1
#define WEB_LIFECYCLE_WIFI_AUTHORITY_ALWAYS_LINKED 1
#define WEB_LIFECYCLE_WEAK
#elif defined(__GNUC__)
#define WEB_LIFECYCLE_HAS_WIFI_AUTHORITY 1
#define WEB_LIFECYCLE_WIFI_AUTHORITY_ALWAYS_LINKED 0
#define WEB_LIFECYCLE_WEAK __attribute__((weak))
#else
#define WEB_LIFECYCLE_HAS_WIFI_AUTHORITY 0
#define WEB_LIFECYCLE_WIFI_AUTHORITY_ALWAYS_LINKED 0
#define WEB_LIFECYCLE_WEAK
#endif

#if WEB_LIFECYCLE_HAS_WIFI_AUTHORITY
extern bool wifi_manager_is_running(void) WEB_LIFECYCLE_WEAK;
extern bool wifi_manager_is_setup_mode(void) WEB_LIFECYCLE_WEAK;
extern bool wifi_manager_is_radio_state_uncertain(void) WEB_LIFECYCLE_WEAK;
extern bool wifi_manager_network_events_suppressed(void) WEB_LIFECYCLE_WEAK;
extern bool wifi_manager_sta_has_ip(void) WEB_LIFECYCLE_WEAK;
#endif


static bool web_lifecycle_wifi_authority_available(void)
{
#if WEB_LIFECYCLE_WIFI_AUTHORITY_ALWAYS_LINKED
    return true;
#elif WEB_LIFECYCLE_HAS_WIFI_AUTHORITY
    return
        wifi_manager_is_running != NULL &&
        wifi_manager_is_setup_mode != NULL &&
        wifi_manager_is_radio_state_uncertain != NULL &&
        wifi_manager_network_events_suppressed != NULL &&
        wifi_manager_sta_has_ip != NULL;
#else
    return false;
#endif
}


web_lifecycle_state_t web_lifecycle_effective_network_state(
    web_lifecycle_state_t event_state,
    bool radio_state_uncertain
)
{
    if (web_lifecycle_wifi_authority_available()) {
#if WEB_LIFECYCLE_HAS_WIFI_AUTHORITY
        const bool uncertain =
            radio_state_uncertain ||
            wifi_manager_is_radio_state_uncertain();

        if (
            uncertain ||
            !wifi_manager_is_running()
        ) {
            return WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
        }

        /* Raw Wi-Fi/IP callbacks are observations only until the manager has
         * atomically published a final target/stopped state. This suppression
         * boundary is narrower than transition_active: the completion callback
         * may run while the serialized transaction is still unwinding. */
        if (wifi_manager_network_events_suppressed()) {
            return WEB_LIFECYCLE_STATE_QUIESCED;
        }

        if (wifi_manager_is_setup_mode()) {
            return WEB_LIFECYCLE_STATE_SETUP_AP;
        }

        return
            wifi_manager_sta_has_ip()
                ? WEB_LIFECYCLE_STATE_STA_CONNECTED
                : WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
#endif
    }

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
        WEB_LIFECYCLE_ACTION_HTTP_STOP,
        WEB_LIFECYCLE_ACTION_HTTPS_START,
        WEB_LIFECYCLE_ACTION_MDNS_START,
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
