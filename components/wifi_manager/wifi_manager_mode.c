#include "wifi_manager.h"

#include "app_event_bus.h"
#include "wifi_manager_internal.h"
#include "wifi_manager_runtime.h"
#include "wifi_manager_sta.h"

#include "esp_log.h"
#include "esp_wifi.h"


static const char *TAG =
    "WIFI_MANAGER";


static esp_err_t wifi_manager_stop_radio(void)
{
    wifi_manager_sta_reset_state();

    esp_err_t err =
        esp_wifi_stop();

    if (
        err == ESP_OK ||
        err == ESP_ERR_WIFI_NOT_STARTED
    ) {
        return ESP_OK;
    }

    return err;
}


esp_err_t wifi_manager_enter_setup_mode(void)
{
    if (
        !wifi_manager_internal_initialized()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        wifi_manager_internal_setup_mode()
    ) {
        return ESP_OK;
    }

    esp_err_t err =
        wifi_manager_stop_radio();

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_runtime_clear_setup_client();

    err =
        wifi_manager_internal_start_setup_mode();

    if (err == ESP_OK) {
        wifi_manager_internal_set_running(
            true
        );
    }

    return err;
}


esp_err_t wifi_manager_connect_saved(void)
{
    if (
        !wifi_manager_internal_initialized()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        wifi_manager_stop_radio();

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_runtime_clear_setup_client();

    wifi_manager_internal_set_setup_mode(
        false
    );

    err =
        wifi_manager_sta_start();

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_internal_set_running(
        true
    );

    ESP_LOGI(
        TAG,
        "Switching to saved Wi-Fi STA credentials"
    );

    (void)app_event_bus_publish(
        APP_EVENT_WIFI_STA_CONNECTING,
        0
    );

    return ESP_OK;
}


esp_err_t wifi_manager_stop(void)
{
    if (
        !wifi_manager_is_running()
    ) {
        return ESP_OK;
    }

    esp_err_t err =
        wifi_manager_stop_radio();

    wifi_manager_runtime_clear_setup_client();

    wifi_manager_internal_set_running(
        false
    );

    wifi_manager_internal_set_setup_mode(
        false
    );

    return err;
}
