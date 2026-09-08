#include "wifi_manager_platform.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"


static const char *TAG =
    "WIFI_PLATFORM";


/*
 * Static Wi-Fi RX buffers live in DMA-capable internal SRAM. The runtime also
 * needs that scarce memory class for the SPI LCD and SDSPI 512-byte bounce
 * buffer. Four static RX buffers are enough for this low-throughput local
 * dashboard and return 3.2 KiB of contiguous DMA-capable SRAM versus six.
 */
#define WIFI_MANAGER_STATIC_RX_BUFFERS  4


static esp_netif_t *s_ap_netif =
    NULL;


static esp_err_t wifi_manager_platform_init_nvs(void)
{
    esp_err_t err =
        nvs_flash_init();

    if (
        err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        err =
            nvs_flash_init();
    }

    return err;
}


esp_err_t wifi_manager_platform_init(void)
{
    esp_err_t err =
        wifi_manager_platform_init_nvs();

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_netif_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_event_loop_create_default();

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        return err;
    }

    if (
        esp_netif_create_default_wifi_sta() ==
        NULL
    ) {
        return ESP_FAIL;
    }

    s_ap_netif =
        esp_netif_create_default_wifi_ap();

    if (s_ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t config =
        WIFI_INIT_CONFIG_DEFAULT();

    config.static_rx_buf_num =
        WIFI_MANAGER_STATIC_RX_BUFFERS;

    ESP_LOGI(
        TAG,
        "Wi-Fi init: static RX DMA buffers=%d",
        config.static_rx_buf_num
    );

    err =
        esp_wifi_init(
            &config
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        esp_wifi_set_storage(
            WIFI_STORAGE_FLASH
        );
}


esp_netif_t *wifi_manager_platform_ap_netif(void)
{
    return s_ap_netif;
}
