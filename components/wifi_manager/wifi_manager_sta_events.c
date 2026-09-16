#include "wifi_manager_sta_events.h"

#include <stdint.h>

#include "app_events.h"
#include "wifi_manager_event_policy.h"
#include "wifi_manager_platform.h"
#include "wifi_manager_runtime.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"


static const char *TAG = "WIFI_STA";


typedef struct
{
    bool initialized;
    bool connect_enabled;
    bool has_ip;
    bool disconnect_reported;
    uint32_t generation;

} wifi_manager_sta_event_state_t;


typedef struct
{
    bool enabled;
    uint32_t generation;

} wifi_manager_sta_admission_t;


static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static wifi_manager_sta_event_state_t s_state;


static wifi_manager_sta_admission_t wifi_manager_sta_admission_snapshot(void)
{
    wifi_manager_sta_admission_t snapshot;

    portENTER_CRITICAL(&s_state_lock);
    snapshot.enabled = s_state.connect_enabled;
    snapshot.generation = s_state.generation;
    portEXIT_CRITICAL(&s_state_lock);

    return snapshot;
}


static bool wifi_manager_sta_admission_still_matches(
    const wifi_manager_sta_admission_t *started
)
{
    if (started == NULL) {
        return false;
    }

    bool accepted;
    portENTER_CRITICAL(&s_state_lock);
    accepted = wifi_manager_event_epoch_matches(
        started->enabled,
        started->generation,
        s_state.connect_enabled,
        s_state.generation
    );
    portEXIT_CRITICAL(&s_state_lock);
    return accepted;
}


static bool wifi_manager_sta_live_association_query(
    bool *associated
)
{
    if (associated == NULL) {
        return false;
    }

    *associated = false;

    wifi_ap_record_t ap_info = {0};
    const esp_err_t err =
        esp_wifi_sta_get_ap_info(&ap_info);

    if (err == ESP_OK) {
        *associated = true;
        return true;
    }

    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        return true;
    }

    ESP_LOGW(
        TAG,
        "Unable to validate live STA association: %s",
        esp_err_to_name(err)
    );
    return false;
}


static bool wifi_manager_sta_live_ip(
    esp_ip4_addr_t *ip
)
{
    if (ip == NULL) {
        return false;
    }

    esp_netif_t *sta_netif =
        wifi_manager_platform_sta_netif();
    if (sta_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    const esp_err_t err =
        esp_netif_get_ip_info(
            sta_netif,
            &ip_info
        );
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to validate live STA IP: %s",
            esp_err_to_name(err)
        );
        return false;
    }

    if (ip_info.ip.addr == 0U) {
        return false;
    }

    *ip = ip_info.ip;
    return true;
}


static void wifi_manager_sta_handle_start(void)
{
    const wifi_manager_sta_admission_t started =
        wifi_manager_sta_admission_snapshot();

    if (!started.enabled) {
        ESP_LOGI(
            TAG,
            "Station idle during setup AP mode"
        );
        return;
    }

    if (!wifi_manager_sta_admission_still_matches(&started)) {
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
    const wifi_manager_sta_admission_t started =
        wifi_manager_sta_admission_snapshot();
    if (!started.enabled) {
        return;
    }

    bool live_associated = false;
    if (!wifi_manager_sta_live_association_query(&live_associated)) {
        return;
    }

    bool was_connected = false;
    bool report_disconnect = false;
    bool accepted = false;

    portENTER_CRITICAL(&s_state_lock);
    accepted = wifi_manager_sta_disconnect_observation_accept(
        started.enabled,
        started.generation,
        live_associated,
        s_state.connect_enabled,
        s_state.generation
    );
    if (accepted) {
        was_connected = s_state.has_ip;
        report_disconnect = !s_state.disconnect_reported;
        s_state.has_ip = false;
        s_state.disconnect_reported = true;
        /* All STA runtime-IP mutations are serialized by the event-state
         * lock first, including reset(), so a retired handler cannot race a
         * newer epoch into restoring old runtime identity. */
        wifi_manager_runtime_clear_sta_ip();
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!accepted) {
        if (live_associated) {
            ESP_LOGI(
                TAG,
                "Ignoring stale STA disconnect; station is currently associated"
            );
        }
        return;
    }

    if (report_disconnect) {
        ESP_LOGW(
            TAG,
            "Station disconnected; reconnecting"
        );
    }

    if (was_connected) {
        (void)app_events_post(
            APP_EVENT_WIFI_STA_DISCONNECTED,
            0
        );
    }

    if (wifi_manager_sta_admission_still_matches(&started)) {
        (void)esp_wifi_connect();
    }
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

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
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
    (void)event_data;

    const wifi_manager_sta_admission_t started =
        wifi_manager_sta_admission_snapshot();
    if (!started.enabled) {
        return;
    }

    bool live_associated = false;
    if (!wifi_manager_sta_live_association_query(&live_associated)) {
        return;
    }

    esp_ip4_addr_t live_ip = {0};
    const bool live_ip_valid =
        live_associated &&
        wifi_manager_sta_live_ip(&live_ip);

    bool accepted = false;
    portENTER_CRITICAL(&s_state_lock);
    accepted = wifi_manager_sta_ip_observation_accept(
        started.enabled,
        started.generation,
        live_associated,
        live_ip_valid,
        s_state.connect_enabled,
        s_state.generation
    );
    if (accepted) {
        s_state.has_ip = true;
        s_state.disconnect_reported = false;
        /* Publish the current netif IP, never event_data from the queued
         * historical GOT_IP callback. */
        wifi_manager_runtime_set_sta_ip(&live_ip);
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!accepted) {
        ESP_LOGI(
            TAG,
            "Ignoring stale STA GOT_IP observation"
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "Connected, IP=" IPSTR,
        IP2STR(&live_ip)
    );

    if (wifi_manager_sta_admission_still_matches(&started)) {
        (void)app_events_post(
            APP_EVENT_WIFI_STA_CONNECTED,
            0
        );
    }
}


esp_err_t wifi_manager_sta_events_init(void)
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
        portENTER_CRITICAL(&s_state_lock);
        s_state.initialized = true;
        portEXIT_CRITICAL(&s_state_lock);
    }

    return err;
}


void wifi_manager_sta_events_set_connect_enabled(
    bool enabled
)
{
    portENTER_CRITICAL(&s_state_lock);
    ++s_state.generation;
    s_state.connect_enabled = enabled;
    s_state.has_ip = false;
    s_state.disconnect_reported = false;
    wifi_manager_runtime_clear_sta_ip();
    portEXIT_CRITICAL(&s_state_lock);
}


void wifi_manager_sta_events_reset(void)
{
    wifi_manager_sta_events_set_connect_enabled(false);
}
