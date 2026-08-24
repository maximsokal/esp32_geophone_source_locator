#include "power_control.h"

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "power_button_monitor.h"


static const char *TAG =
    "POWER_CONTROL";


#define OFF_PULSE_MS  200


static esp_err_t power_control_init_off_output(void)
{
    const gpio_config_t off_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_POWER_OFF_GPIO,

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &off_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure OFF GPIO: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        gpio_set_level(
            BOARD_POWER_OFF_GPIO,
            0
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set OFF GPIO LOW: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    return ESP_OK;
}


esp_err_t power_control_init(void)
{
    esp_err_t err =
        power_control_init_off_output();

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

    gpio_set_level(
        BOARD_POWER_OFF_GPIO,
        1
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            OFF_PULSE_MS
        )
    );

    /*
     * With Pololu-only power the ESP32 should already
     * be physically off before reaching this point.
     *
     * If USB still powers the ESP32, return OFF LOW.
     */
    gpio_set_level(
        BOARD_POWER_OFF_GPIO,
        0
    );

    ESP_LOGW(
        TAG,
        "ESP32 still powered - probably USB power is connected"
    );
}
