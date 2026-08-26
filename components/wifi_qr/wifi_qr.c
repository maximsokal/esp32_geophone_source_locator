#include "wifi_qr.h"

#include <stdio.h>

#include "app_config.h"
#include "display.h"
#include "wifi_qr_render.h"

#include "esp_log.h"

#include "qrcode.h"


static const char *TAG =
    "WIFI_QR";


#define WIFI_QR_MAX_VERSION    10


static esp_err_t wifi_qr_draw_payload(
    const char *payload,
    const char *label,
    int x,
    int y,
    int max_size
)
{
    if (
        payload == NULL ||
        label == NULL ||
        x < 0 ||
        y < 0 ||
        max_size <= 0 ||
        x + max_size > DISPLAY_WIDTH ||
        y + max_size > DISPLAY_HEIGHT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_log_level_set(
        "QRCODE",
        ESP_LOG_WARN
    );

    wifi_qr_render_context_t context = {
        .x = x,
        .y = y,
        .max_size = max_size,
        .result = ESP_FAIL,
    };

    esp_qrcode_config_t config =
        ESP_QRCODE_CONFIG_DEFAULT();

    config.display_func_with_cb =
        wifi_qr_render_callback;

    config.user_data =
        &context;

    config.max_qrcode_version =
        WIFI_QR_MAX_VERSION;

    config.qrcode_ecc_level =
        ESP_QRCODE_ECC_LOW;

    ESP_LOGI(
        TAG,
        "Generating %s QR",
        label
    );

    esp_err_t err =
        esp_qrcode_generate(
            &config,
            payload
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "QR generation failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    return context.result;
}


esp_err_t wifi_qr_draw(
    int x,
    int y,
    int max_size
)
{
    char payload[128];

    const int length =
        snprintf(
            payload,
            sizeof(payload),
            "WIFI:T:WPA;S:%s;P:%s;;",
            APP_WIFI_AP_SSID,
            APP_WIFI_AP_PASSWORD
        );

    if (
        length < 0 ||
        length >= (int)sizeof(payload)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return
        wifi_qr_draw_payload(
            payload,
            "setup Wi-Fi",
            x,
            y,
            max_size
        );
}


esp_err_t wifi_qr_draw_url(
    const char *url,
    int x,
    int y,
    int max_size
)
{
    return
        wifi_qr_draw_payload(
            url,
            "URL",
            x,
            y,
            max_size
        );
}
