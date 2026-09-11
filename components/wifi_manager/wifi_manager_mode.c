#include "wifi_manager.h"

#include "app_event_bus.h"
#include "wifi_manager_internal.h"
#include "wifi_manager_runtime.h"
#include "wifi_manager_sta.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"


static const char *TAG =
    "WIFI_MANAGER";

static portMUX_TYPE s_radio_stop_callback_lock =
    portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_radio_stop_prepare_callback_t s_radio_stop_prepare_callback =
    NULL;
static void *s_radio_stop_prepare_context =
    NULL;
static unsigned s_radio_stop_prepare_in_flight = 0U;


esp_err_t wifi_manager_set_radio_stop_prepare_callback(
    wifi_manager_radio_stop_prepare_callback_t callback,
    void *context
)
{
    if (callback == NULL && context != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    s_radio_stop_prepare_callback = callback;
    s_radio_stop_prepare_context = context;
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    if (callback == NULL) {
        for (;;) {
            unsigned in_flight;
            portENTER_CRITICAL(&s_radio_stop_callback_lock);
            in_flight = s_radio_stop_prepare_in_flight;
            portEXIT_CRITICAL(&s_radio_stop_callback_lock);
            if (in_flight == 0U) break;
            vTaskDelay(1U);
        }
    }

    return ESP_OK;
}


static esp_err_t wifi_manager_prepare_radio_stop(void)
{
    wifi_manager_radio_stop_prepare_callback_t callback;
    void *context;

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    callback = s_radio_stop_prepare_callback;
    context = s_radio_stop_prepare_context;
    if (callback != NULL) {
        ++s_radio_stop_prepare_in_flight;
    }
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    if (callback == NULL) {
        return ESP_OK;
    }

    const esp_err_t err = callback(context);

    portENTER_CRITICAL(&s_radio_stop_callback_lock);
    --s_radio_stop_prepare_in_flight;
    portEXIT_CRITICAL(&s_radio_stop_callback_lock);

    return err;
}


static esp_err_t wifi_manager_stop_radio(void)
{
    /*
     * Network services own their lifecycle. The prepare callback is a
     * synchronous barrier: it must return only after those services have
     * released resources that must not survive the Wi-Fi mode transition.
     */
    esp_err_t err =
        wifi_manager_prepare_radio_stop();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Network lifecycle did not quiesce before radio stop: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    wifi_manager_sta_reset_state();

    err =
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

    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_runtime_clear_setup_client();

    wifi_manager_internal_set_running(
        false
    );

    wifi_manager_internal_set_setup_mode(
        false
    );

    return ESP_OK;
}
