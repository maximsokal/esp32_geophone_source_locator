#include "power_control.h"

#include "board_config.h"

#include "esp_log.h"

#include "power_button_monitor.h"
#include "power_output.h"


static const char *TAG =
    "POWER_CONTROL";


esp_err_t power_control_init(void)
{
    esp_err_t err =
        power_output_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        power_button_monitor_init();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize power button monitor: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "Initialized: OFF=GPIO%d BUTTON=GPIO%d",
        BOARD_POWER_OFF_GPIO,
        BOARD_POWER_BUTTON_GPIO
    );

    return ESP_OK;
}


void power_control_wait_for_shutdown_request(void)
{
    power_button_monitor_wait_for_shutdown_request();
}


bool power_control_is_shutdown_requested(void)
{
    return
        power_button_monitor_is_shutdown_requested();
}


void power_control_shutdown(void)
{
    power_button_monitor_stop();

    ESP_LOGW(
        TAG,
        "Sending OFF pulse to Pololu"
    );

    power_output_pulse_off();

    ESP_LOGW(
        TAG,
        "ESP32 still powered - probably USB power is connected"
    );
}
