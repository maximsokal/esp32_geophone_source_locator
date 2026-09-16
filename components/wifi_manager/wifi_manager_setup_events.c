#include "wifi_manager_setup_events.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_events.h"
#include "wifi_manager_event_policy.h"
#include "wifi_manager_profile.h"
#include "wifi_manager_runtime.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"


static const char *TAG =
    "WIFI_SETUP_CLIENT";


typedef struct
{
    bool initialized;
    bool enabled;
    uint32_t generation;

} wifi_manager_setup_event_state_t;


typedef struct
{
    bool enabled;
    uint32_t generation;

} wifi_manager_setup_admission_t;


static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static wifi_manager_setup_event_state_t s_state;


static wifi_manager_setup_admission_t wifi_manager_setup_admission_snapshot(void)
{
    wifi_manager_setup_admission_t snapshot;

    portENTER_CRITICAL(&s_state_lock);
    snapshot.enabled = s_state.enabled;
    snapshot.generation = s_state.generation;
    portEXIT_CRITICAL(&s_state_lock);

    return snapshot;
}


static bool wifi_manager_setup_live_client_query(
    const uint8_t mac[6],
    bool *client_present,
    bool *any_client_present
)
{
    if (
        client_present == NULL ||
        any_client_present == NULL
    ) {
        return false;
    }

    *client_present = false;
    *any_client_present = false;

    wifi_sta_list_t stations = {0};
    const esp_err_t err =
        esp_wifi_ap_get_sta_list(&stations);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to validate live setup client list: %s",
            esp_err_to_name(err)
        );
        return false;
    }

    *any_client_present = stations.num > 0;
    if (mac == NULL) {
        return true;
    }

    for (size_t i = 0U; i < (size_t)stations.num; ++i) {
        if (memcmp(stations.sta[i].mac, mac, 6U) == 0) {
            *client_present = true;
            break;
        }
    }

    return true;
}


static void wifi_manager_setup_known_client(
    const uint8_t mac[6],
    bool *known
)
{
    if (known == NULL) {
        return;
    }

    *known = false;
    const esp_err_t err =
        wifi_manager_profile_is_known_client(
            mac,
            known
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Known-client lookup failed: %s",
            esp_err_to_name(err)
        );
    }
}


static bool wifi_manager_setup_commit_client_if_current(
    const wifi_manager_setup_admission_t *started,
    const uint8_t mac[6],
    bool live_client_present,
    bool known
)
{
    if (started == NULL || mac == NULL) {
        return false;
    }

    bool accepted;
    portENTER_CRITICAL(&s_state_lock);
    accepted = wifi_manager_setup_client_observation_accept(
        started->enabled,
        started->generation,
        live_client_present,
        s_state.enabled,
        s_state.generation
    );
    if (accepted) {
        /* setup reset/commit paths use the same event lock first, so an old
         * handler cannot repopulate runtime state after its epoch retires. */
        wifi_manager_runtime_set_setup_client(
            mac,
            known
        );
    }
    portEXIT_CRITICAL(&s_state_lock);

    return accepted;
}


static void wifi_manager_setup_handle_client_present(
    const uint8_t mac[6],
    bool publish_ready,
    const esp_ip4_addr_t *assigned_ip
)
{
    if (mac == NULL) {
        return;
    }

    const wifi_manager_setup_admission_t started =
        wifi_manager_setup_admission_snapshot();
    if (!started.enabled) {
        return;
    }

    bool live_client_present = false;
    bool any_client_present = false;
    if (!wifi_manager_setup_live_client_query(
            mac,
            &live_client_present,
            &any_client_present
        )) {
        return;
    }
    (void)any_client_present;

    if (!live_client_present) {
        ESP_LOGI(
            TAG,
            "Ignoring stale setup-client observation; event MAC is not currently associated"
        );
        return;
    }

    bool known = false;
    wifi_manager_setup_known_client(
        mac,
        &known
    );

    if (!wifi_manager_setup_commit_client_if_current(
            &started,
            mac,
            live_client_present,
            known
        )) {
        ESP_LOGI(
            TAG,
            "Ignoring stale setup-client observation"
        );
        return;
    }

    if (publish_ready && assigned_ip != NULL) {
        ESP_LOGI(
            TAG,
            "Setup client ready: "
            "%02X:%02X:%02X:%02X:%02X:%02X IP=" IPSTR,
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5],
            IP2STR(assigned_ip)
        );

        const wifi_manager_setup_admission_t current =
            wifi_manager_setup_admission_snapshot();
        if (wifi_manager_event_epoch_matches(
                started.enabled,
                started.generation,
                current.enabled,
                current.generation
            )) {
            (void)app_events_post(
                APP_EVENT_WIFI_SETUP_CLIENT_READY,
                0
            );
        }
        return;
    }

    ESP_LOGI(
        TAG,
        "Setup client associated: "
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
}


static void wifi_manager_setup_handle_disconnect(void)
{
    const wifi_manager_setup_admission_t started =
        wifi_manager_setup_admission_snapshot();
    if (!started.enabled) {
        return;
    }

    bool ignored_client_present = false;
    bool any_client_present = false;
    if (!wifi_manager_setup_live_client_query(
            NULL,
            &ignored_client_present,
            &any_client_present
        )) {
        return;
    }

    bool accepted;
    portENTER_CRITICAL(&s_state_lock);
    accepted = wifi_manager_setup_disconnect_observation_accept(
        started.enabled,
        started.generation,
        any_client_present,
        s_state.enabled,
        s_state.generation
    );
    if (accepted) {
        wifi_manager_runtime_clear_setup_client();
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!accepted) {
        if (any_client_present) {
            ESP_LOGI(
                TAG,
                "Ignoring stale setup-client disconnect; a client is currently associated"
            );
        }
        return;
    }

    ESP_LOGI(
        TAG,
        "Setup client disconnected"
    );

    const wifi_manager_setup_admission_t current =
        wifi_manager_setup_admission_snapshot();
    if (wifi_manager_event_epoch_matches(
            started.enabled,
            started.generation,
            current.enabled,
            current.generation
        )) {
        (void)app_events_post(
            APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED,
            0
        );
    }
}


static void wifi_manager_setup_wifi_event(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;

    if (
        event_id ==
        WIFI_EVENT_AP_STACONNECTED
    ) {
        const wifi_event_ap_staconnected_t *event =
            (const wifi_event_ap_staconnected_t *)
                event_data;

        if (event == NULL) {
            return;
        }

        wifi_manager_setup_handle_client_present(
            event->mac,
            false,
            NULL
        );
        return;
    }

    if (
        event_id ==
        WIFI_EVENT_AP_STADISCONNECTED
    ) {
        wifi_manager_setup_handle_disconnect();
    }
}


static void wifi_manager_setup_ip_event(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;
    (void)event_id;

    const ip_event_ap_staipassigned_t *event =
        (const ip_event_ap_staipassigned_t *)
            event_data;

    if (event == NULL) {
        return;
    }

    wifi_manager_setup_handle_client_present(
        event->mac,
        true,
        &event->ip
    );
}


esp_err_t wifi_manager_setup_events_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_state.initialized;
    portEXIT_CRITICAL(&s_state_lock);

    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err =
        esp_event_handler_register(
            WIFI_EVENT,
            WIFI_EVENT_AP_STACONNECTED,
            wifi_manager_setup_wifi_event,
            NULL
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_event_handler_register(
            WIFI_EVENT,
            WIFI_EVENT_AP_STADISCONNECTED,
            wifi_manager_setup_wifi_event,
            NULL
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_AP_STAIPASSIGNED,
            wifi_manager_setup_ip_event,
            NULL
        );

    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.initialized = true;
        portEXIT_CRITICAL(&s_state_lock);
    }

    return err;
}


void wifi_manager_setup_events_set_enabled(
    bool enabled
)
{
    portENTER_CRITICAL(&s_state_lock);
    ++s_state.generation;
    s_state.enabled = enabled;
    if (!enabled) {
        wifi_manager_runtime_clear_setup_client();
    }
    portEXIT_CRITICAL(&s_state_lock);
}


void wifi_manager_setup_events_reset(void)
{
    wifi_manager_setup_events_set_enabled(false);
}
