#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "wifi_manager_mdns.h"
#include "wifi_manager_platform.h"

static esp_err_t s_nvs_init_result;
static esp_err_t s_nvs_second_init_result;
static unsigned s_nvs_init_calls;
static unsigned s_nvs_erase_calls;
static esp_err_t s_netif_init_result;
static esp_err_t s_event_loop_result;
static esp_netif_t s_sta_netif;
static esp_netif_t s_ap_netif;
static bool s_sta_create_ok;
static bool s_ap_create_ok;
static esp_err_t s_wifi_init_result;
static esp_err_t s_wifi_storage_result;
static int s_last_rx_buffers;
static unsigned s_wifi_init_calls;
static unsigned s_wifi_storage_calls;
static esp_err_t s_mdns_init_result;
static esp_err_t s_mdns_hostname_result;
static esp_err_t s_mdns_instance_result;
static esp_err_t s_mdns_service_result;
static unsigned s_mdns_init_calls;
static unsigned s_mdns_hostname_calls;
static unsigned s_mdns_instance_calls;
static unsigned s_mdns_service_calls;
static unsigned s_mdns_free_calls;
static char s_hostname[32];
static char s_instance_name[32];
static char s_service[16];
static char s_protocol[16];
static uint16_t s_port;

static void reset_fakes(void)
{
    s_nvs_init_result = ESP_OK;
    s_nvs_second_init_result = ESP_OK;
    s_nvs_init_calls = 0U;
    s_nvs_erase_calls = 0U;
    s_netif_init_result = ESP_OK;
    s_event_loop_result = ESP_OK;
    s_sta_create_ok = true;
    s_ap_create_ok = true;
    s_wifi_init_result = ESP_OK;
    s_wifi_storage_result = ESP_OK;
    s_last_rx_buffers = 0;
    s_wifi_init_calls = 0U;
    s_wifi_storage_calls = 0U;
    s_mdns_init_result = ESP_OK;
    s_mdns_hostname_result = ESP_OK;
    s_mdns_instance_result = ESP_OK;
    s_mdns_service_result = ESP_OK;
    s_mdns_init_calls = 0U;
    s_mdns_hostname_calls = 0U;
    s_mdns_instance_calls = 0U;
    s_mdns_service_calls = 0U;
    s_mdns_free_calls = 0U;
    s_hostname[0] = '\0';
    s_instance_name[0] = '\0';
    s_service[0] = '\0';
    s_protocol[0] = '\0';
    s_port = 0U;
}

esp_err_t nvs_flash_init(void)
{
    ++s_nvs_init_calls;
    return s_nvs_init_calls == 1U ? s_nvs_init_result : s_nvs_second_init_result;
}

esp_err_t nvs_flash_erase(void)
{
    ++s_nvs_erase_calls;
    return ESP_OK;
}

esp_err_t esp_netif_init(void)
{
    return s_netif_init_result;
}

esp_err_t esp_event_loop_create_default(void)
{
    return s_event_loop_result;
}

esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return s_sta_create_ok ? &s_sta_netif : NULL;
}

esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    return s_ap_create_ok ? &s_ap_netif : NULL;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    assert(config != NULL);
    ++s_wifi_init_calls;
    s_last_rx_buffers = config->static_rx_buf_num;
    return s_wifi_init_result;
}

esp_err_t esp_wifi_set_storage(int storage)
{
    assert(storage == WIFI_STORAGE_FLASH);
    ++s_wifi_storage_calls;
    return s_wifi_storage_result;
}

esp_err_t mdns_init(void)
{
    ++s_mdns_init_calls;
    return s_mdns_init_result;
}

esp_err_t mdns_hostname_set(const char *hostname)
{
    ++s_mdns_hostname_calls;
    snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);
    return s_mdns_hostname_result;
}

esp_err_t mdns_instance_name_set(const char *instance_name)
{
    ++s_mdns_instance_calls;
    snprintf(s_instance_name, sizeof(s_instance_name), "%s", instance_name);
    return s_mdns_instance_result;
}

esp_err_t mdns_service_add(
    const char *instance_name,
    const char *service,
    const char *protocol,
    uint16_t port,
    void *txt,
    size_t num_txt
)
{
    assert(instance_name == NULL);
    assert(txt == NULL);
    assert(num_txt == 0U);
    ++s_mdns_service_calls;
    snprintf(s_service, sizeof(s_service), "%s", service);
    snprintf(s_protocol, sizeof(s_protocol), "%s", protocol);
    s_port = port;
    return s_mdns_service_result;
}

void mdns_free(void)
{
    ++s_mdns_free_calls;
}

static void test_platform_recovery_and_dependency_failures(void)
{
    reset_fakes();
    s_nvs_init_result = ESP_ERR_NVS_NO_FREE_PAGES;
    assert(wifi_manager_platform_init() == ESP_OK);
    assert(s_nvs_init_calls == 2U);
    assert(s_nvs_erase_calls == 1U);
    assert(s_last_rx_buffers == 4);
    assert(s_wifi_init_calls == 1U);
    assert(s_wifi_storage_calls == 1U);
    assert(wifi_manager_platform_sta_netif() == &s_sta_netif);
    assert(wifi_manager_platform_ap_netif() == &s_ap_netif);

    reset_fakes();
    s_nvs_init_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(s_wifi_init_calls == 0U);

    reset_fakes();
    s_netif_init_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(s_wifi_init_calls == 0U);

    reset_fakes();
    s_event_loop_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(s_wifi_init_calls == 0U);

    reset_fakes();
    s_event_loop_result = ESP_ERR_INVALID_STATE;
    s_sta_create_ok = false;
    assert(wifi_manager_platform_init() == ESP_FAIL);

    reset_fakes();
    s_ap_create_ok = false;
    assert(wifi_manager_platform_init() == ESP_FAIL);

    reset_fakes();
    s_wifi_init_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(s_wifi_storage_calls == 0U);

    reset_fakes();
    s_wifi_storage_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(s_wifi_storage_calls == 1U);
}

static void test_mdns_lifecycle_and_cleanup(void)
{
    reset_fakes();
    wifi_manager_mdns_stop();
    assert(!wifi_manager_mdns_is_ready());
    assert(wifi_manager_mdns_init() == ESP_OK);
    assert(wifi_manager_mdns_is_ready());
    assert(s_mdns_init_calls == 1U);
    assert(s_mdns_hostname_calls == 1U);
    assert(strcmp(s_hostname, APP_MDNS_HOSTNAME) == 0);
    assert(s_mdns_instance_calls == 1U);
    assert(strcmp(s_instance_name, APP_MDNS_INSTANCE_NAME) == 0);
    assert(s_mdns_service_calls == 1U);
    assert(strcmp(s_service, "_https") == 0);
    assert(strcmp(s_protocol, "_tcp") == 0);
    assert(s_port == APP_WEB_HTTPS_PORT);
    assert(wifi_manager_mdns_init() == ESP_OK);
    assert(s_mdns_init_calls == 1U);
    wifi_manager_mdns_stop();
    assert(!wifi_manager_mdns_is_ready());
    assert(s_mdns_free_calls == 1U);
    wifi_manager_mdns_stop();
    assert(s_mdns_free_calls == 1U);

    reset_fakes();
    s_mdns_hostname_result = ESP_FAIL;
    assert(wifi_manager_mdns_init() == ESP_FAIL);
    assert(!wifi_manager_mdns_is_ready());
    assert(s_mdns_free_calls == 1U);

    reset_fakes();
    s_mdns_instance_result = ESP_FAIL;
    assert(wifi_manager_mdns_init() == ESP_FAIL);
    assert(!wifi_manager_mdns_is_ready());
    assert(s_mdns_free_calls == 1U);

    reset_fakes();
    s_mdns_service_result = ESP_FAIL;
    assert(wifi_manager_mdns_init() == ESP_FAIL);
    assert(!wifi_manager_mdns_is_ready());
    assert(s_mdns_free_calls == 1U);
}

int main(void)
{
    test_platform_recovery_and_dependency_failures();
    test_mdns_lifecycle_and_cleanup();
    puts("wifi manager platform and mDNS tests passed");
    return 0;
}
