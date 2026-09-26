#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "wifi_manager_internal.h"
#include "wifi_manager_platform.h"
#include "wifi_manager_runtime.h"
#include "wifi_manager_setup_events.h"
#include "wifi_manager_sta_events.h"

typedef struct
{
    esp_event_base_t base;
    int32_t event_id;
    esp_event_handler_t handler;
    void *context;
} registered_handler_t;

struct esp_timer {
    esp_timer_create_args_t args;
    uint64_t timeout_us;
    bool created;
    bool active;
};

static registered_handler_t s_handlers[8];
static size_t s_handler_count;
static size_t s_registration_calls;
static size_t s_registration_failure_call;
static size_t s_unregistration_calls;

static esp_netif_t s_sta_netif;
static esp_err_t s_sta_association_result;
static esp_err_t s_ip_info_result;
static esp_ip4_addr_t s_live_ip;
static esp_err_t s_ap_station_list_result;
static wifi_sta_list_t s_live_stations;
static bool s_known_client;
static esp_err_t s_known_client_result;
static unsigned s_connect_calls;
static unsigned s_app_event_calls;
static unsigned s_sta_connected_events;
static unsigned s_sta_disconnected_events;
static unsigned s_setup_ready_events;
static unsigned s_setup_disconnected_events;
static app_event_type_t s_last_event;
static struct esp_timer s_reconnect_timer;

void wifi_manager_internal_get_state(wifi_manager_internal_state_t *state)
{
    assert(state != NULL);
    memset(state, 0, sizeof(*state));
}

static void reset_observations(void)
{
    s_sta_association_result = ESP_ERR_WIFI_NOT_CONNECT;
    s_ip_info_result = ESP_OK;
    s_live_ip.addr = 0U;
    s_ap_station_list_result = ESP_OK;
    memset(&s_live_stations, 0, sizeof(s_live_stations));
    s_known_client = false;
    s_known_client_result = ESP_OK;
    s_connect_calls = 0U;
    s_app_event_calls = 0U;
    s_sta_connected_events = 0U;
    s_sta_disconnected_events = 0U;
    s_setup_ready_events = 0U;
    s_setup_disconnected_events = 0U;
    s_last_event = APP_EVENT_WIFI_STA_CONNECTED;
    wifi_manager_sta_events_reset();
    wifi_manager_setup_events_reset();
    wifi_manager_runtime_clear_setup_client();
    wifi_manager_runtime_clear_sta_ip();
}

esp_err_t esp_timer_create(
    const esp_timer_create_args_t *create_args,
    esp_timer_handle_t *out_handle
)
{
    assert(create_args != NULL);
    assert(create_args->callback != NULL);
    assert(out_handle != NULL);
    s_reconnect_timer.args = *create_args;
    s_reconnect_timer.timeout_us = 0U;
    s_reconnect_timer.created = true;
    s_reconnect_timer.active = false;
    *out_handle = &s_reconnect_timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(
    esp_timer_handle_t timer,
    uint64_t timeout_us
)
{
    assert(timer == &s_reconnect_timer);
    assert(timer->created);
    assert(timeout_us > 0U);
    timer->timeout_us = timeout_us;
    timer->active = true;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer == &s_reconnect_timer);
    timer->active = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer == &s_reconnect_timer);
    timer->created = false;
    timer->active = false;
    return ESP_OK;
}

void esp_timer_test_fire(void)
{
    assert(s_reconnect_timer.created);
    s_reconnect_timer.active = false;
    s_reconnect_timer.args.callback(s_reconnect_timer.args.arg);
}

esp_err_t esp_event_handler_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t handler,
    void *context
)
{
    ++s_registration_calls;
    if (s_registration_failure_call != 0U &&
        s_registration_calls == s_registration_failure_call) {
        return ESP_FAIL;
    }
    assert(s_handler_count < sizeof(s_handlers) / sizeof(s_handlers[0]));
    s_handlers[s_handler_count++] = (registered_handler_t) {
        .base = event_base,
        .event_id = event_id,
        .handler = handler,
        .context = context,
    };
    return ESP_OK;
}

esp_err_t esp_event_handler_unregister(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t handler
)
{
    ++s_unregistration_calls;
    for (size_t i = 0U; i < s_handler_count; ++i) {
        if (strcmp(s_handlers[i].base, event_base) == 0 &&
            s_handlers[i].event_id == event_id &&
            s_handlers[i].handler == handler) {
            memmove(
                &s_handlers[i],
                &s_handlers[i + 1U],
                (s_handler_count - i - 1U) * sizeof(s_handlers[0])
            );
            --s_handler_count;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static registered_handler_t *find_handler(
    esp_event_base_t base,
    int32_t event_id
)
{
    for (size_t i = 0U; i < s_handler_count; ++i) {
        if (strcmp(s_handlers[i].base, base) == 0 &&
            s_handlers[i].event_id == event_id) {
            return &s_handlers[i];
        }
    }
    return NULL;
}

static void dispatch(
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
)
{
    registered_handler_t *registered = find_handler(base, event_id);
    if (registered == NULL) {
        registered = find_handler(base, ESP_EVENT_ANY_ID);
    }
    assert(registered != NULL);
    registered->handler(
        registered->context,
        base,
        event_id,
        event_data
    );
}

esp_netif_t *wifi_manager_platform_sta_netif(void)
{
    return &s_sta_netif;
}

esp_err_t esp_wifi_connect(void)
{
    ++s_connect_calls;
    return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    if (s_sta_association_result == ESP_OK && ap_info != NULL) {
        memset(ap_info, 0xA5, sizeof(*ap_info));
    }
    return s_sta_association_result;
}

esp_err_t esp_netif_get_ip_info(
    esp_netif_t *netif,
    esp_netif_ip_info_t *ip_info
)
{
    assert(netif == &s_sta_netif);
    if (s_ip_info_result == ESP_OK && ip_info != NULL) {
        ip_info->ip = s_live_ip;
    }
    return s_ip_info_result;
}

esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *sta_list)
{
    if (s_ap_station_list_result == ESP_OK && sta_list != NULL) {
        *sta_list = s_live_stations;
    }
    return s_ap_station_list_result;
}

esp_err_t wifi_manager_profile_is_known_client(
    const uint8_t mac[6],
    bool *known
)
{
    assert(mac != NULL);
    assert(known != NULL);
    *known = s_known_client;
    return s_known_client_result;
}

esp_err_t app_events_post(app_event_type_t type, int32_t value)
{
    (void)value;
    ++s_app_event_calls;
    s_last_event = type;
    switch (type) {
        case APP_EVENT_WIFI_STA_CONNECTED: ++s_sta_connected_events; break;
        case APP_EVENT_WIFI_STA_DISCONNECTED: ++s_sta_disconnected_events; break;
        case APP_EVENT_WIFI_SETUP_CLIENT_READY: ++s_setup_ready_events; break;
        case APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED: ++s_setup_disconnected_events; break;
        default: assert(false); break;
    }
    return ESP_OK;
}

static void init_event_registrations(void)
{
    s_registration_failure_call = 2U;
    assert(wifi_manager_sta_events_init() == ESP_FAIL);
    assert(s_registration_calls == 2U);
    assert(s_unregistration_calls == 1U);
    assert(s_handler_count == 0U);
    s_registration_failure_call = 0U;
    assert(wifi_manager_sta_events_init() == ESP_OK);
    assert(s_registration_calls == 4U);
    assert(s_handler_count == 2U);
    assert(wifi_manager_sta_events_init() == ESP_OK);
    assert(s_registration_calls == 4U);

    s_registration_failure_call = 7U;
    assert(wifi_manager_setup_events_init() == ESP_FAIL);
    assert(s_registration_calls == 7U);
    assert(s_unregistration_calls == 3U);
    assert(s_handler_count == 2U);
    s_registration_failure_call = 0U;
    assert(wifi_manager_setup_events_init() == ESP_OK);
    assert(s_registration_calls == 10U);
    assert(s_handler_count == 5U);
    assert(wifi_manager_setup_events_init() == ESP_OK);
    assert(s_registration_calls == 10U);
}

static void test_sta_start_ip_disconnect_reconnect(void)
{
    reset_observations();
    wifi_manager_sta_events_set_connect_enabled(true);
    s_sta_association_result = ESP_OK;
    s_live_ip.addr = 0x04030201U;

    dispatch(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(s_connect_calls == 1U);

    dispatch(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_sta_connected_events == 1U);
    assert(s_last_event == APP_EVENT_WIFI_STA_CONNECTED);

    char ip_text[16];
    assert(wifi_manager_runtime_sta_ip_string(ip_text, sizeof(ip_text)) == ESP_OK);
    assert(strcmp(ip_text, "1.2.3.4") == 0);

    s_sta_association_result = ESP_ERR_WIFI_NOT_CONNECT;
    dispatch(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(s_sta_disconnected_events == 1U);
    assert(s_connect_calls == 2U);
    assert(wifi_manager_runtime_sta_ip_string(ip_text, sizeof(ip_text)) == ESP_ERR_NOT_FOUND);
    wifi_manager_reconnect_status_t reconnect_status = {0};
    assert(wifi_manager_get_reconnect_status(&reconnect_status) == ESP_OK);
    assert(reconnect_status.last_reason == WIFI_REASON_UNSPECIFIED);
    assert(reconnect_status.retry_count == 1U);
    assert(reconnect_status.next_delay_ms == 0U);

    const wifi_event_sta_disconnected_t transient = {
        .reason = WIFI_REASON_BEACON_TIMEOUT
    };
    dispatch(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&transient);
    assert(s_sta_disconnected_events == 1U);
    assert(s_connect_calls == 2U);
    assert(s_reconnect_timer.active);
    assert(s_reconnect_timer.timeout_us == 500000U);
    assert(wifi_manager_get_reconnect_status(&reconnect_status) == ESP_OK);
    assert(reconnect_status.retry_count == 2U);
    assert(reconnect_status.next_delay_ms == 500U);

    esp_timer_test_fire();
    assert(s_connect_calls == 3U);
    assert(!s_reconnect_timer.active);
    assert(wifi_manager_get_reconnect_status(&reconnect_status) == ESP_OK);
    assert(reconnect_status.next_delay_ms == 0U);

    const wifi_event_sta_disconnected_t authentication_failure = {
        .reason = WIFI_REASON_AUTH_FAIL
    };
    dispatch(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        (void *)&authentication_failure
    );
    assert(s_connect_calls == 3U);
    assert(s_reconnect_timer.active);
    assert(s_reconnect_timer.timeout_us == 30000000U);
    assert(wifi_manager_get_reconnect_status(&reconnect_status) == ESP_OK);
    assert(reconnect_status.last_reason == WIFI_REASON_AUTH_FAIL);
    assert(reconnect_status.retry_count == 3U);
    assert(reconnect_status.next_delay_ms == 30000U);

    s_sta_association_result = ESP_OK;
    s_live_ip.addr = 0x04030201U;
    dispatch(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!s_reconnect_timer.active);
    assert(wifi_manager_get_reconnect_status(&reconnect_status) == ESP_OK);
    assert(reconnect_status.retry_count == 0U);
    assert(reconnect_status.next_delay_ms == 0U);

    s_sta_association_result = ESP_ERR_WIFI_NOT_CONNECT;
    dispatch(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        (void *)&authentication_failure
    );
    assert(s_reconnect_timer.active);
    assert(s_reconnect_timer.timeout_us == 30000000U);
    wifi_manager_sta_events_set_connect_enabled(true);
    assert(!s_reconnect_timer.active);
    esp_timer_test_fire();
    assert(s_connect_calls == 3U);

    dispatch(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        (void *)&authentication_failure
    );
    assert(s_reconnect_timer.active);
    assert(s_reconnect_timer.timeout_us == 60000000U);
    assert(wifi_manager_get_reconnect_status(&reconnect_status) == ESP_OK);
    assert(reconnect_status.retry_count == 2U);
    assert(reconnect_status.next_delay_ms == 60000U);
    wifi_manager_sta_events_set_connect_enabled(false);
    assert(!s_reconnect_timer.active);
    esp_timer_test_fire();
    assert(s_connect_calls == 3U);
}

static void test_sta_rejects_stale_or_invalid_observations(void)
{
    reset_observations();
    wifi_manager_sta_events_set_connect_enabled(true);
    s_sta_association_result = ESP_OK;
    s_live_ip.addr = 0U;
    dispatch(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_app_event_calls == 0U);
    assert(!wifi_manager_sta_has_ip());

    s_ip_info_result = ESP_FAIL;
    dispatch(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_app_event_calls == 0U);

    s_sta_association_result = ESP_OK;
    s_ip_info_result = ESP_OK;
    s_live_ip.addr = 0x08070605U;
    dispatch(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_sta_connected_events == 1U);

    wifi_manager_sta_events_reset();
    assert(!wifi_manager_sta_has_ip());
    dispatch(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_sta_connected_events == 1U);
}

static void test_setup_client_presence_ready_and_disconnect(void)
{
    reset_observations();
    const uint8_t mac[6] = { 0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U };
    memcpy(s_live_stations.sta[0].mac, mac, sizeof(mac));
    s_live_stations.num = 1U;
    s_known_client = true;
    wifi_manager_setup_events_set_enabled(true);

    struct {
        uint8_t mac[6];
    } connected = { { 0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U } };
    dispatch(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &connected);
    assert(wifi_manager_runtime_setup_client_connected());
    assert(wifi_manager_runtime_setup_client_known());

    uint8_t saved_mac[6];
    assert(wifi_manager_runtime_setup_client_mac(saved_mac) == ESP_OK);
    assert(memcmp(saved_mac, mac, sizeof(mac)) == 0);

    struct {
        uint8_t mac[6];
        esp_ip4_addr_t ip;
    } assigned = { { 0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U }, { 0x0100007FU } };
    dispatch(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &assigned);
    assert(s_setup_ready_events == 1U);

    s_live_stations.num = 1U;
    dispatch(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, NULL);
    assert(s_setup_disconnected_events == 0U);
    assert(wifi_manager_runtime_setup_client_connected());

    s_live_stations.num = 0U;
    dispatch(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, NULL);
    assert(s_setup_disconnected_events == 1U);
    assert(!wifi_manager_runtime_setup_client_connected());
}

static void test_setup_ignores_stale_client_and_disabled_events(void)
{
    reset_observations();
    const uint8_t event_mac[6] = { 1U, 2U, 3U, 4U, 5U, 6U };
    struct {
        uint8_t mac[6];
    } connected;
    memcpy(connected.mac, event_mac, sizeof(event_mac));
    s_live_stations.num = 0U;
    wifi_manager_setup_events_set_enabled(true);
    dispatch(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &connected);
    assert(!wifi_manager_runtime_setup_client_connected());
    assert(s_app_event_calls == 0U);

    wifi_manager_setup_events_reset();
    s_live_stations.num = 1U;
    memcpy(s_live_stations.sta[0].mac, event_mac, sizeof(event_mac));
    dispatch(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &connected);
    assert(!wifi_manager_runtime_setup_client_connected());
    assert(s_app_event_calls == 0U);
}

int main(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    s_handler_count = 0U;
    s_registration_calls = 0U;
    s_registration_failure_call = 0U;
    s_unregistration_calls = 0U;
    init_event_registrations();
    test_sta_start_ip_disconnect_reconnect();
    test_sta_rejects_stale_or_invalid_observations();
    test_setup_client_presence_ready_and_disconnect();
    test_setup_ignores_stale_client_and_disabled_events();
    puts("wifi manager event runtime tests passed");
    return 0;
}
