#include "web_server_internal.h"

#include "app_event_bus.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "WEB_WIFI_SETUP";


#define WIFI_SETUP_BODY_SIZE        256
#define WIFI_SETUP_SSID_SIZE        33
#define WIFI_SETUP_PASSWORD_SIZE    64
#define WIFI_SETUP_STA_SWITCH_MS    750


esp_err_t web_wifi_setup_send_page(
    httpd_req_t *request
)
{
    return web_wifi_setup_page_send_form(request);
}


static esp_err_t web_wifi_setup_post_handler(
    httpd_req_t *request
)
{
    if (
        !wifi_manager_is_setup_mode()
    ) {
        httpd_resp_send_err(
            request,
            HTTPD_403_FORBIDDEN,
            "Wi-Fi setup is only available in setup mode"
        );

        return ESP_ERR_INVALID_STATE;
    }

    if (
        !web_https_onboarding_ca_install_confirmed()
    ) {
        httpd_resp_send_err(
            request,
            HTTPD_403_FORBIDDEN,
            "Confirm GeoLocator Root CA installation before saving Wi-Fi settings"
        );

        return ESP_ERR_INVALID_STATE;
    }

    char body[WIFI_SETUP_BODY_SIZE];

    esp_err_t err =
        web_wifi_setup_request_read_body(
            request,
            body,
            sizeof(body)
        );

    if (err != ESP_OK) {
        httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid form body"
        );

        return err;
    }

    char ssid[WIFI_SETUP_SSID_SIZE];
    char password[WIFI_SETUP_PASSWORD_SIZE];

    err =
        web_wifi_setup_form_parse(
            body,
            ssid,
            sizeof(ssid),
            password,
            sizeof(password)
        );

    if (err != ESP_OK) {
        httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid Wi-Fi settings"
        );

        return err;
    }

    err =
        wifi_manager_save_sta_credentials(
            ssid,
            password
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to save Wi-Fi credentials: %s",
            esp_err_to_name(err)
        );

        httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to save Wi-Fi settings"
        );

        return err;
    }

    err = web_wifi_setup_page_send_saved(request);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi setup saved");

    (void)app_event_bus_publish(
        APP_EVENT_WIFI_CREDENTIALS_SAVED,
        0
    );

    /*
     * httpd_resp_send() has already sent the confirmation page. Keep the
     * SoftAP alive briefly so the phone can render it, then perform the
     * promised APSTA -> STA transition. Doing this here also avoids making
     * the UI layer responsible for Wi-Fi control.
     */
    vTaskDelay(
        pdMS_TO_TICKS(WIFI_SETUP_STA_SWITCH_MS)
    );

    err =
        wifi_manager_connect_saved();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to switch from setup AP to saved STA: %s",
            esp_err_to_name(err)
        );

        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Setup complete; switched to saved Wi-Fi STA"
    );

    return ESP_OK;
}


esp_err_t web_wifi_setup_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t open_uri = {
        .uri = "/setup",
        .method = HTTP_GET,
        .handler = web_wifi_setup_page_open_handler,
        .user_ctx = NULL,
    };

    esp_err_t err =
        httpd_register_uri_handler(
            server,
            &open_uri
        );

    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t save_uri = {
        .uri = "/api/wifi/setup",
        .method = HTTP_POST,
        .handler = web_wifi_setup_post_handler,
        .user_ctx = NULL,
    };

    return httpd_register_uri_handler(
        server,
        &save_uri
    );
}
