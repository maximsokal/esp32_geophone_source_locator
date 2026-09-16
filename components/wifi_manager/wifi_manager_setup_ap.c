#include "wifi_manager_setup_ap.h"

#include <string.h>

#include "app_config.h"
#include "wifi_manager_setup_events.h"
#include "wifi_manager_sta_events.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"


static const char *TAG =
    "WIFI_SETUP_AP";


static void wifi_manager_setup_log_dma_heap(
    const char *stage
)
{
    const uint32_t caps =
        MALLOC_CAP_DMA |
        MALLOC_CAP_INTERNAL;

    ESP_LOGI(
        TAG,
        "Internal DMA heap %s: free=%u largest=%u",
        stage,
        (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps)
    );
}


esp_err_t wifi_manager_setup_ap_start(
    esp_netif_t *ap_netif
)
{
    if (ap_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        wifi_manager_setup_events_init();

    if (err != ESP_OK) {
        return err;
    }

    /* Setup starts with both event lifetimes retired. APSTA keeps a physical
     * STA interface, but application STA connect handling remains disabled. */
    wifi_manager_setup_events_reset();
    wifi_manager_sta_events_set_connect_enabled(
        false
    );

    wifi_config_t config = {0};

    strlcpy(
        (char *)config.ap.ssid,
        APP_WIFI_AP_SSID,
        sizeof(config.ap.ssid)
    );

    strlcpy(
        (char *)config.ap.password,
        APP_WIFI_AP_PASSWORD,
        sizeof(config.ap.password)
    );

    config.ap.ssid_len =
        strlen(
            APP_WIFI_AP_SSID
        );

    config.ap.channel =
        1;

    /* One setup phone is enough and keeps SoftAP memory deterministic. */
    config.ap.max_connection =
        1;

    config.ap.authmode =
        WIFI_AUTH_WPA2_PSK;

    wifi_manager_setup_log_dma_heap(
        "before APSTA start"
    );

    /*
     * Keep APSTA here because STA credentials are written while the setup
     * portal is still active. wifi_manager_connect_saved() later performs
     * the clean APSTA -> STA transition after the HTTP response is sent.
     */
    err =
        esp_wifi_set_mode(
            WIFI_MODE_APSTA
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_wifi_set_config(
            WIFI_IF_AP,
            &config
        );

    if (err != ESP_OK) {
        return err;
    }

    /* AP client events may arrive as soon as the target radio starts. */
    wifi_manager_setup_events_set_enabled(
        true
    );

    err =
        esp_wifi_start();

    if (err != ESP_OK) {
        wifi_manager_setup_events_reset();
        return err;
    }

    wifi_manager_setup_log_dma_heap(
        "after APSTA start"
    );

    ESP_LOGI(
        TAG,
        "Setup AP started: %s",
        APP_WIFI_AP_SSID
    );

    ESP_LOGI(
        TAG,
        "Setup page: %s",
        APP_WIFI_SETUP_URL
    );

    return ESP_OK;
}
