#include "wifi_manager_setup_events.h"

#include <stdbool.h>

#include "app_events.h"
#include "wifi_manager_profile.h"
#include "wifi_manager_runtime.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"


static const char *TAG =
    "WIFI_SETUP_CLIENT";


static bool s_initialized =
    false;


static void wifi_manager_setup_track_client(
    const uint8_t mac[6]
)
{
    bool known =
        false;

    esp_err_t err =
        wifi_manager_profile_is_known_client(
            mac,
            &known
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Known-client lookup failed: %s",
            esp_err_to_name(err)
        );
    }

    wifi_manager_runtime_set_setup_client(
        mac,
        known
    );
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

        wifi_manager_setup_track_client(
            event->mac
        );

        ESP_LOGI(
            TAG,
            "Setup client associated: "
            "%02X:%02X:%02X:%02X:%02X:%02X",
            event->mac[0],
            event->mac[1],
            event->mac[2],
            event->mac[3],
            event->mac[4],
            event->mac[5]
        );

        return;
    }

    if (
        event_id ==
        WIFI_EVENT_AP_STADISCONNECTED
    ) {
        wifi_manager_runtime_clear_setup_client();

        ESP_LOGI(
            TAG,
            "Setup client disconnected"
        );

        (void)app_events_post(
            APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED,
            0
        );
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

    wifi_manager_setup_track_client(
        event->mac
    );

    ESP_LOGI(
        TAG,
        "Setup client ready: "
        "%02X:%02X:%02X:%02X:%02X:%02X IP=" IPSTR,
        event->mac[0],
        event->mac[1],
        event->mac[2],
        event->mac[3],
        event->mac[4],
        event->mac[5],
        IP2STR(&event->ip)
    );

    (void)
        app_events_post(
            APP_EVENT_WIFI_SETUP_CLIENT_READY,
            0
        );
}


esp_err_t wifi_manager_setup_events_init(void)
{
    if (s_initialized) {
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
        s_initialized =
            true;
    }

    return err;
}
