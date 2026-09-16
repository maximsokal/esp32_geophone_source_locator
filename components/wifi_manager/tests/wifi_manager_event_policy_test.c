#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "wifi_manager_event_policy.h"


int main(void)
{
    assert(wifi_manager_event_epoch_matches(true, 7U, true, 7U));
    assert(!wifi_manager_event_epoch_matches(false, 7U, true, 7U));
    assert(!wifi_manager_event_epoch_matches(true, 7U, false, 7U));
    assert(!wifi_manager_event_epoch_matches(true, 7U, true, 8U));

    assert(wifi_manager_sta_ip_observation_accept(
        true, 11U, true, true, true, 11U
    ));
    assert(!wifi_manager_sta_ip_observation_accept(
        true, 11U, false, true, true, 11U
    ));
    assert(!wifi_manager_sta_ip_observation_accept(
        true, 11U, true, false, true, 11U
    ));
    assert(!wifi_manager_sta_ip_observation_accept(
        true, 11U, true, true, true, 12U
    ));

    assert(wifi_manager_sta_disconnect_observation_accept(
        true, 13U, false, true, 13U
    ));
    assert(!wifi_manager_sta_disconnect_observation_accept(
        true, 13U, true, true, 13U
    ));
    assert(!wifi_manager_sta_disconnect_observation_accept(
        true, 13U, false, true, 14U
    ));

    assert(wifi_manager_setup_client_observation_accept(
        true, 17U, true, true, 17U
    ));
    assert(!wifi_manager_setup_client_observation_accept(
        true, 17U, false, true, 17U
    ));
    assert(!wifi_manager_setup_client_observation_accept(
        true, 17U, true, true, 18U
    ));

    assert(wifi_manager_setup_disconnect_observation_accept(
        true, 19U, false, true, 19U
    ));
    assert(!wifi_manager_setup_disconnect_observation_accept(
        true, 19U, true, true, 19U
    ));
    assert(!wifi_manager_setup_disconnect_observation_accept(
        true, 19U, false, false, 19U
    ));

    puts("WIFI_MANAGER_EVENT_POLICY_HOST_PASS");
    return 0;
}
