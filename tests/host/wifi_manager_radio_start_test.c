#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_manager_setup_ap.h"
#include "wifi_manager_setup_events.h"
#include "wifi_manager_sta.h"
#include "wifi_manager_sta_events.h"

enum {
    TRACE_STA_RESET = 1,
    TRACE_STA_ENABLE,
    TRACE_SETUP_INIT,
    TRACE_SETUP_RESET,
    TRACE_SETUP_ENABLE,
    TRACE_SET_MODE,
    TRACE_SET_CONFIG,
    TRACE_WIFI_START,
    TRACE_SET_PS,
};

static int s_trace[32];
static size_t s_trace_count;
static esp_err_t s_setup_init_result;
static esp_err_t s_set_mode_result;
static esp_err_t s_set_config_result;
static esp_err_t s_wifi_start_result;
static esp_err_t s_set_ps_result;
static wifi_mode_t s_last_mode;
static wifi_interface_t s_last_interface;
static wifi_config_t s_last_config;
static wifi_ps_type_t s_last_ps;
static bool s_last_sta_connect_enabled;

static void reset_fakes(void)
{
    memset(s_trace, 0, sizeof(s_trace));
    s_trace_count = 0U;
    s_setup_init_result = ESP_OK;
    s_set_mode_result = ESP_OK;
    s_set_config_result = ESP_OK;
    s_wifi_start_result = ESP_OK;
    s_set_ps_result = ESP_OK;
    memset(&s_last_config, 0, sizeof(s_last_config));
    s_last_sta_connect_enabled = false;
}

static void trace_event(int event)
{
    assert(s_trace_count < sizeof(s_trace) / sizeof(s_trace[0]));
    s_trace[s_trace_count++] = event;
}

esp_err_t wifi_manager_sta_events_init(void)
{
    return ESP_OK;
}

void wifi_manager_sta_events_set_connect_enabled(bool enabled)
{
    trace_event(TRACE_STA_ENABLE);
    s_last_sta_connect_enabled = enabled;
}

void wifi_manager_sta_events_reset(void)
{
    trace_event(TRACE_STA_RESET);
}

esp_err_t wifi_manager_setup_events_init(void)
{
    trace_event(TRACE_SETUP_INIT);
    return s_setup_init_result;
}

void wifi_manager_setup_events_set_enabled(bool enabled)
{
    trace_event(TRACE_SETUP_ENABLE);
    assert(enabled);
}

void wifi_manager_setup_events_reset(void)
{
    trace_event(TRACE_SETUP_RESET);
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    trace_event(TRACE_SET_MODE);
    s_last_mode = mode;
    return s_set_mode_result;
}

esp_err_t esp_wifi_set_config(wifi_interface_t interface_id, const wifi_config_t *config)
{
    trace_event(TRACE_SET_CONFIG);
    s_last_interface = interface_id;
    assert(config != NULL);
    s_last_config = *config;
    return s_set_config_result;
}

esp_err_t esp_wifi_start(void)
{
    trace_event(TRACE_WIFI_START);
    return s_wifi_start_result;
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t type)
{
    trace_event(TRACE_SET_PS);
    s_last_ps = type;
    return s_set_ps_result;
}

static void assert_trace(const int *expected, size_t count)
{
    assert(s_trace_count == count);
    assert(memcmp(s_trace, expected, count * sizeof(expected[0])) == 0);
}

static void test_sta_start_order_and_failures(void)
{
    reset_fakes();
    assert(wifi_manager_sta_start() == ESP_OK);
    const int expected[] = {
        TRACE_STA_RESET,
        TRACE_SET_MODE,
        TRACE_STA_ENABLE,
        TRACE_WIFI_START,
        TRACE_SET_PS,
    };
    assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
    assert(s_last_mode == WIFI_MODE_STA);
    assert(s_last_ps == WIFI_PS_MIN_MODEM);

    reset_fakes();
    s_set_mode_result = ESP_FAIL;
    assert(wifi_manager_sta_start() == ESP_FAIL);
    const int mode_failure[] = { TRACE_STA_RESET, TRACE_SET_MODE };
    assert_trace(mode_failure, sizeof(mode_failure) / sizeof(mode_failure[0]));

    reset_fakes();
    s_wifi_start_result = ESP_FAIL;
    assert(wifi_manager_sta_start() == ESP_FAIL);
    const int start_failure[] = {
        TRACE_STA_RESET,
        TRACE_SET_MODE,
        TRACE_STA_ENABLE,
        TRACE_WIFI_START,
        TRACE_STA_RESET,
    };
    assert_trace(start_failure, sizeof(start_failure) / sizeof(start_failure[0]));

    reset_fakes();
    s_set_ps_result = ESP_FAIL;
    assert(wifi_manager_sta_start() == ESP_OK);
    assert(s_trace_count == 5U);
    assert(s_trace[4] == TRACE_SET_PS);
}

static void test_setup_ap_configuration_and_failures(void)
{
    esp_netif_t ap_netif = { 0 };
    reset_fakes();
    assert(wifi_manager_setup_ap_start(&ap_netif) == ESP_OK);
    const int expected[] = {
        TRACE_SETUP_INIT,
        TRACE_SETUP_RESET,
        TRACE_STA_ENABLE,
        TRACE_SET_MODE,
        TRACE_SET_CONFIG,
        TRACE_SETUP_ENABLE,
        TRACE_WIFI_START,
    };
    assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
    assert(s_last_mode == WIFI_MODE_APSTA);
    assert(s_last_interface == WIFI_IF_AP);
    assert(strcmp((const char *)s_last_config.ap.ssid, APP_WIFI_AP_SSID) == 0);
    assert(strcmp((const char *)s_last_config.ap.password, APP_WIFI_AP_PASSWORD) == 0);
    assert(s_last_config.ap.ssid_len == strlen(APP_WIFI_AP_SSID));
    assert(s_last_config.ap.channel == 1U);
    assert(s_last_config.ap.max_connection == 1U);
    assert(s_last_config.ap.authmode == WIFI_AUTH_WPA2_PSK);
    assert(!s_last_sta_connect_enabled);

    reset_fakes();
    assert(wifi_manager_setup_ap_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(s_trace_count == 0U);

    reset_fakes();
    s_setup_init_result = ESP_FAIL;
    assert(wifi_manager_setup_ap_start(&ap_netif) == ESP_FAIL);
    assert(s_trace_count == 1U);
    assert(s_trace[0] == TRACE_SETUP_INIT);

    reset_fakes();
    s_set_mode_result = ESP_FAIL;
    assert(wifi_manager_setup_ap_start(&ap_netif) == ESP_FAIL);
    const int mode_failure[] = {
        TRACE_SETUP_INIT,
        TRACE_SETUP_RESET,
        TRACE_STA_ENABLE,
        TRACE_SET_MODE,
    };
    assert_trace(mode_failure, sizeof(mode_failure) / sizeof(mode_failure[0]));

    reset_fakes();
    s_set_config_result = ESP_FAIL;
    assert(wifi_manager_setup_ap_start(&ap_netif) == ESP_FAIL);
    const int config_failure[] = {
        TRACE_SETUP_INIT,
        TRACE_SETUP_RESET,
        TRACE_STA_ENABLE,
        TRACE_SET_MODE,
        TRACE_SET_CONFIG,
    };
    assert_trace(config_failure, sizeof(config_failure) / sizeof(config_failure[0]));

    reset_fakes();
    s_wifi_start_result = ESP_FAIL;
    assert(wifi_manager_setup_ap_start(&ap_netif) == ESP_FAIL);
    const int start_failure[] = {
        TRACE_SETUP_INIT,
        TRACE_SETUP_RESET,
        TRACE_STA_ENABLE,
        TRACE_SET_MODE,
        TRACE_SET_CONFIG,
        TRACE_SETUP_ENABLE,
        TRACE_WIFI_START,
        TRACE_SETUP_RESET,
    };
    assert_trace(start_failure, sizeof(start_failure) / sizeof(start_failure[0]));
}

int main(void)
{
    test_sta_start_order_and_failures();
    test_setup_ap_configuration_and_failures();
    puts("wifi manager radio start tests passed");
    return 0;
}
