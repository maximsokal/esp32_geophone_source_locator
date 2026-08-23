#include "power_control.h"

#include "board_config.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "POWER_CONTROL";

#define BUTTON_POLL_MS       10
#define BUTTON_DEBOUNCE_MS   50
#define OFF_PULSE_MS         200


esp_err_t power_control_init(void)
{
    /*
     * GPIO18 -> OFF Pololu
     *
     * LOW  = нормальная работа
     * HIGH = команда Pololu выключить питание
     */
    const gpio_config_t off_config = {
        .pin_bit_mask = 1ULL << BOARD_POWER_OFF_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&off_config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure OFF GPIO: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = gpio_set_level(BOARD_POWER_OFF_GPIO, 0);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set OFF GPIO LOW: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /*
     * GPIO21 <- середина делителя 10k / 20k.
     *
     * Кнопка отпущена: LOW
     * Кнопка нажата:   HIGH (~3.3 V)
     *
     * Внешний 20k уже тянет вход к GND,
     * поэтому внутренние подтяжки не нужны.
     */
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOARD_POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&button_config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure button GPIO: %s",
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


static void wait_for_button_release(void)
{
    while (gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 1) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
}


void power_control_wait_for_shutdown_request(void)
{
    /*
     * ESP32 мог загрузиться, пока пользователь всё ещё
     * держит кнопку, которой включил устройство.
     *
     * Поэтому первое нажатие НЕ считаем выключением.
     */
    if (gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 1) {
        ESP_LOGI(TAG, "Waiting for startup button release");

        wait_for_button_release();
    }

    ESP_LOGI(TAG, "Power button armed");

    while (1) {
        if (gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 1) {
            /*
             * Debounce.
             */
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));

            if (gpio_get_level(BOARD_POWER_BUTTON_GPIO) == 1) {
                ESP_LOGI(TAG, "Shutdown button pressed");

                /*
                 * Не посылаем OFF, пока ON ещё удерживается HIGH.
                 * Сначала пользователь должен отпустить кнопку.
                 */
                wait_for_button_release();

                ESP_LOGI(TAG, "Shutdown requested");

                return;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}


void power_control_shutdown(void)
{
    ESP_LOGW(TAG, "Sending OFF pulse to Pololu");

    gpio_set_level(BOARD_POWER_OFF_GPIO, 1);

    vTaskDelay(pdMS_TO_TICKS(OFF_PULSE_MS));

    /*
     * При питании только через Pololu ESP32 уже должен
     * физически отключиться и до этой строки не дойдёт.
     *
     * При подключённом USB ESP32 останется запитанным
     * от компьютера, поэтому возвращаем OFF обратно LOW.
     */
    gpio_set_level(BOARD_POWER_OFF_GPIO, 0);

    ESP_LOGW(
        TAG,
        "ESP32 still powered - probably USB power is connected"
    );
}