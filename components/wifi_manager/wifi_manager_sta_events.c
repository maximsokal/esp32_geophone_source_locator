#include "wifi_manager_sta_events.h"

#include <stdint.h>

#include "app_events.h"
#include "wifi_manager_event_policy.h"
#include "wifi_manager_platform.h"
#include "wifi_manager_reconnect_policy.h"
#include "wifi_manager_runtime.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
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
    wifi_err_reason_t last_reason;
    uint32_t retry_count;
    uint32_t same_reason_streak;
    uint32_t next_delay_ms;
    uint32_t retry_generation;
    bool retry_pending;

} wifi_manager_sta_event_state_t;


typedef struct
{
    bool enabled;
    uint32_t generation;

} wifi_manager_sta_admission_t;


static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static wifi_manager_sta_event_state_t s_state;
static esp_timer_handle_t s_reconnect_timer;


static void wifi_manager_sta_stop_reconnect_timer(bool was_pending)
{
    if (was_pending && s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
}


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


static void wifi_manager_sta_reconnect_timer_callback(void *context)
{
    (void)context;
    wifi_manager_sta_admission_t started = {0};
    bool pending = false;

    portENTER_CRITICAL(&s_state_lock);
    pending = s_state.retry_pending;
    started.enabled = s_state.connect_enabled;
    started.generation = s_state.retry_generation;
    if (pending) {
        s_state.retry_pending = false;
        s_state.next_delay_ms = 0U;
    }
    pending = pending && started.enabled &&
        started.generation == s_state.generation;
    portEXIT_CRITICAL(&s_state_lock);

    if (!pending || !wifi_manager_sta_admission_still_matches(&started)) {
        return;
    }

    (void)esp_wifi_connect();
}


static void wifi_manager_sta_request_reconnect(
    const wifi_manager_sta_admission_t *started,
    const wifi_manager_reconnect_decision_t *decision
)
{
    if (started == NULL || decision == NULL) {
        return;
    }

    bool accepted = false;
    bool was_pending = false;
    portENTER_CRITICAL(&s_state_lock);
    accepted = decision->generation == s_state.generation &&
        wifi_manager_event_epoch_matches(
            started->enabled,
            started->generation,
            s_state.connect_enabled,
            s_state.generation
        );
    if (accepted) {
        was_pending = s_state.retry_pending;
        s_state.retry_generation = decision->generation;
        s_state.retry_pending = decision->delay_ms > 0U;
        s_state.next_delay_ms = decision->delay_ms;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!accepted) {
        return;
    }

    wifi_manager_sta_stop_reconnect_timer(was_pending);
    if (decision->delay_ms == 0U) {
        if (wifi_manager_sta_admission_still_matches(started)) {
            (void)esp_wifi_connect();
        }
        return;
    }

    const uint64_t delay_us = (uint64_t)decision->delay_ms * 1000U;
    const esp_err_t err = s_reconnect_timer == NULL
        ? ESP_ERR_INVALID_STATE
        : esp_timer_start_once(s_reconnect_timer, delay_us);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Reconnect timer unavailable; connecting immediately: %s",
            esp_err_to_name(err)
        );

        portENTER_CRITICAL(&s_state_lock);
        accepted = s_state.retry_pending &&
            s_state.retry_generation == decision->generation &&
            s_state.generation == decision->generation;
        if (accepted) {
            s_state.retry_pending = false;
            s_state.next_delay_ms = 0U;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (accepted && wifi_manager_sta_admission_still_matches(started)) {
            (void)esp_wifi_connect();
        }
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    accepted = s_state.retry_pending &&
        s_state.retry_generation == decision->generation &&
        s_state.connect_enabled &&
        s_state.generation == decision->generation;
    portEXIT_CRITICAL(&s_state_lock);
    if (!accepted) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
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


static void wifi_manager_sta_handle_disconnect(wifi_err_reason_t reason)
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
    uint32_t retry_count = 0U;
    wifi_manager_reconnect_decision_t decision = {0};

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
        if (s_state.retry_count < UINT32_MAX) {
            ++s_state.retry_count;
        }
        if (s_state.same_reason_streak > 0U &&
            s_state.last_reason == reason) {
            if (s_state.same_reason_streak < UINT32_MAX) {
                ++s_state.same_reason_streak;
            }
        }
        else {
            s_state.same_reason_streak = 1U;
        }
        s_state.last_reason = reason;
        retry_count = s_state.retry_count;
        decision = wifi_manager_reconnect_policy_decide(
            reason,
            s_state.retry_count,
            s_state.same_reason_streak,
            started.generation
        );
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
            "Station disconnected (reason=%d retry=%u delay=%u ms)",
            (int)reason,
            (unsigned)retry_count,
            (unsigned)decision.delay_ms
        );
    }

    if (was_connected) {
        (void)app_events_post(
            APP_EVENT_WIFI_STA_DISCONNECTED,
            0
        );
    }

    wifi_manager_sta_request_reconnect(&started, &decision);
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

    if (event_id == WIFI_EVENT_STA_START) {
        wifi_manager_sta_handle_start();
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_err_reason_t reason = event_data == NULL
            ? WIFI_REASON_UNSPECIFIED
            : ((const wifi_event_sta_disconnected_t *)event_data)->reason;
        wifi_manager_sta_handle_disconnect(reason);
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
    bool retry_was_pending = false;
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
        s_state.retry_count = 0U;
        s_state.same_reason_streak = 0U;
        retry_was_pending = s_state.retry_pending;
        s_state.retry_pending = false;
        s_state.next_delay_ms = 0U;
        /* Publish the current netif IP, never event_data from the queued
         * historical GOT_IP callback. */
        wifi_manager_runtime_set_sta_ip(&live_ip);
    }
    portEXIT_CRITICAL(&s_state_lock);

    wifi_manager_sta_stop_reconnect_timer(retry_was_pending);

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

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_manager_sta_reconnect_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sta_reconnect"
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_manager_sta_on_wifi_event,
            NULL
        );

    if (err != ESP_OK) {
        (void)esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
        return err;
    }

    err =
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_manager_sta_on_ip_event,
            NULL
        );

    if (err != ESP_OK) {
        (void)esp_event_handler_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_manager_sta_on_wifi_event
        );
        (void)esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
        return err;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state.initialized = true;
    portEXIT_CRITICAL(&s_state_lock);

    return err;
}


void wifi_manager_sta_events_set_connect_enabled(
    bool enabled
)
{
    bool was_pending = false;
    portENTER_CRITICAL(&s_state_lock);
    ++s_state.generation;
    s_state.connect_enabled = enabled;
    s_state.has_ip = false;
    s_state.disconnect_reported = false;
    was_pending = s_state.retry_pending;
    s_state.retry_pending = false;
    s_state.next_delay_ms = 0U;
    wifi_manager_runtime_clear_sta_ip();
    portEXIT_CRITICAL(&s_state_lock);

    wifi_manager_sta_stop_reconnect_timer(was_pending);
}


void wifi_manager_sta_events_reset(void)
{
    wifi_manager_sta_events_set_connect_enabled(false);
}


void wifi_manager_sta_events_get_reconnect_status(
    int32_t *last_reason,
    uint32_t *retry_count,
    uint32_t *next_delay_ms
)
{
    portENTER_CRITICAL(&s_state_lock);
    if (last_reason != NULL) {
        *last_reason = (int32_t)s_state.last_reason;
    }
    if (retry_count != NULL) {
        *retry_count = s_state.retry_count;
    }
    if (next_delay_ms != NULL) {
        *next_delay_ms = s_state.next_delay_ms;
    }
    portEXIT_CRITICAL(&s_state_lock);
}
