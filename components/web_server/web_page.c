#include "web_server_internal.h"

#include <stddef.h>
#include <string.h>

#include "app_config.h"


#define WIFI_SSID_TOKEN  "{{WIFI_SSID}}"


static esp_err_t web_page_send_chunk(
    httpd_req_t *request,
    const char *data,
    size_t length
)
{
    if (length == 0) {
        return ESP_OK;
    }

    return
        httpd_resp_send_chunk(
            request,
            data,
            length
        );
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
        web_page_send_chunk(
            request,
            html,
            (size_t)(
                token -
                html
            )
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        httpd_resp_send_chunk(
            request,
            APP_WIFI_AP_SSID,
            HTTPD_RESP_USE_STRLEN
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
        httpd_resp_send_chunk(
            request,
            suffix,
            HTTPD_RESP_USE_STRLEN
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        httpd_resp_send_chunk(
            request,
            NULL,
            0
        );
}
