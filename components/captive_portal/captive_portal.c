#include "captive_portal.h"

#include "captive_dns_server.h"
#include "captive_http.h"

#include "web_server.h"

#include "esp_log.h"


static const char *TAG =
    "CAPTIVE_PORTAL";


static bool s_running =
    false;


esp_err_t captive_portal_start(void)
{
    if (s_running) {

        ESP_LOGW(
            TAG,
            "Captive portal already running"
        );

        return ESP_OK;
    }


    if (!web_server_is_running()) {

        ESP_LOGE(
            TAG,
            "HTTP server must be running first"
        );

        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Start DNS first.
     */
    esp_err_t err =
        captive_dns_server_start();


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to start DNS redirect: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /*
     * Redirect every unknown HTTP URI to the root page.
     */
    err =
        web_server_set_not_found_handler(
            captive_http_not_found_handler
        );


    if (err != ESP_OK) {

        captive_dns_server_stop();

        ESP_LOGE(
            TAG,
            "Failed to install HTTP redirect handler: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /*
     * Captive portal probes intentionally request unknown
     * host names and paths. Suppress expected HTTP warnings.
     */
    esp_log_level_set(
        "httpd_uri",
        ESP_LOG_ERROR
    );


    esp_log_level_set(
        "httpd_txrx",
        ESP_LOG_ERROR
    );


    esp_log_level_set(
        "httpd_parse",
        ESP_LOG_ERROR
    );


    s_running =
        true;


    ESP_LOGI(
        TAG,
        "Captive portal started"
    );


    return ESP_OK;
}


esp_err_t captive_portal_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }


    esp_err_t err =
        web_server_clear_not_found_handler();


    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Failed to clear HTTP redirect handler: %s",
            esp_err_to_name(err)
        );
    }


    captive_dns_server_stop();


    s_running =
        false;


    ESP_LOGI(
        TAG,
        "Captive portal stopped"
    );


    return err;
}


bool captive_portal_is_running(void)
{
    return s_running;
}