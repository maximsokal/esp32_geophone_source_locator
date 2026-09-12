#include "wifi_manager_sta_events.h"

#include "app_events.h"
#include "wifi_manager_runtime.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"


static const char *TAG = "WIFI_STA";

static bool s_initialized = false;
static bool s_connect_enabled = false;
static bool s_has_ip = false;
static bool s_disconnect_reported = false;


static void wifi_manager_sta_handle_start(void)
{
    if (!s_connect_enabled) {
        ESP_LOGI(
            TAG,
            "Station idle during setup AP mode"
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "Station started, connecting"
    );

    (void)esp_wifi_connect();
}


static void wifi_manager_sta_handle_disconnect(void)
{
    const bool was_connected =
        s_has_ip;

    s_has_ip = false;

    wifi_manager_runtime_clear_sta_ip();

    if (!s_disconnect_reported) {
        ESP_LOGW(
            TAG,
            "Station disconnected; reconnecting"
        );

        s_disconnect_reported = true;
    }

    if (was_connected) {
        (void)app_events_post(
            APP_EVENT_WIFI_STA_DISCONNECTED,
            0
        );
    }

    (void)esp_wifi_connect();
}


static void wifi_manager_sta_on_wifi_event(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_STA_START) {
        wifi_manager_sta_handle_start();
        return;
    }

    if (
        event_id == WIFI_EVENT_STA_DISCONNECTED &&
        s_connect_enabled
    ) {
        wifi_manager_sta_handle_disconnect();
    }
}


static void wifi_manager_sta_on_ip_event(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event =
        (const ip_event_got_ip_t *)event_data;

    if (event == NULL) {
        return;
    }

    s_has_ip = true;
    s_disconnect_reported = false;

    wifi_manager_runtime_set_sta_ip(
        &event->ip_info.ip
    );

    ESP_LOGI(
        TAG,
        "Connected, IP=" IPSTR,
        IP2STR(&event->ip_info.ip)
    );

    (void)app_events_post(
        APP_EVENT_WIFI_STA_CONNECTED,
        0
    );
}


esp_err_t wifi_manager_sta_events_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err =
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_manager_sta_on_wifi_event,
            NULL
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_manager_sta_on_ip_event,
            NULL
        );

    if (err == ESP_OK) {
        s_initialized = true;
    }

    return err;
}


void wifi_manager_sta_events_set_connect_enabled(
    bool enabled
)
{
    s_connect_enabled = enabled;

    if (enabled) {
        s_disconnect_reported = false;
    }
}


void wifi_manager_sta_events_reset(void)
{
    s_connect_enabled = false;
    s_has_ip = false;
    s_disconnect_reported = false;

    wifi_manager_runtime_clear_sta_ip();
}
