#include "web_server_internal.h"

#include <stddef.h>
#include <string.h>

#include "app_config.h"

#include "esp_log.h"


#define WIFI_SSID_TOKEN       "{{WIFI_SSID}}"
#define WEB_PAGE_CHUNK_BYTES  512U


static const char *TAG =
    "WEB_PAGE";


static esp_err_t web_page_send_data(
    httpd_req_t *request,
    const char *data,
    size_t length,
    const char *label
)
{
    if (
        request == NULL ||
        data == NULL ||
        label == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset =
        0;

    while (offset < length) {
        const size_t remaining =
            length - offset;

        const size_t chunk_length =
            remaining > WEB_PAGE_CHUNK_BYTES
                ? WEB_PAGE_CHUNK_BYTES
                : remaining;

        const esp_err_t err =
            httpd_resp_send_chunk(
                request,
                data + offset,
                chunk_length
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to send %s chunk at offset=%u length=%u: %s",
                label,
                (unsigned)offset,
                (unsigned)chunk_length,
                esp_err_to_name(err)
            );

            return err;
        }

        offset +=
            chunk_length;
    }

    return ESP_OK;
}


esp_err_t web_page_send_index(
    httpd_req_t *request
)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *html =
        web_static_asset_data(
            WEB_STATIC_ASSET_INDEX_HTML
        );

    if (html == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *token =
        strstr(
            html,
            WIFI_SSID_TOKEN
        );

    if (token == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    esp_err_t err =
        web_page_send_data(
            request,
            html,
            (size_t)(
                token -
                html
            ),
            "index-prefix"
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        web_page_send_data(
            request,
            APP_WIFI_AP_SSID,
            strlen(APP_WIFI_AP_SSID),
            "wifi-ssid"
        );

    if (err != ESP_OK) {
        return err;
    }

    const char *suffix =
        token +
        strlen(
            WIFI_SSID_TOKEN
        );

    err =
        web_page_send_data(
            request,
            suffix,
            strlen(suffix),
            "index-suffix"
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        httpd_resp_send_chunk(
            request,
            NULL,
            0
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to finalize index response: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "Index response completed"
    );

    return ESP_OK;
}
