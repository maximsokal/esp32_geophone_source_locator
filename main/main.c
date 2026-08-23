#include "esp_err.h"
#include "esp_log.h"

#include "spi_bus.h"
#include "sd_card.h"
#include "display.h"
#include "power_control.h"
#include "keypad.h"


static const char *TAG = "APP";


void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Geophone Source Locator");

    /*
     * Power control.
     */
    ESP_ERROR_CHECK(power_control_init());

    /*
     * Analog 5-button keypad.
     */
    ESP_ERROR_CHECK(keypad_init());

    /*
     * ВРЕМЕННО:
     * выводим RAW/mV для калибровки пяти кнопок.
     */
    ESP_ERROR_CHECK(keypad_start_event_monitor());

    /*
     * Shared SPI2 bus: TFT + microSD.
     */
    ESP_ERROR_CHECK(app_spi_bus_init());

    /*
     * microSD first.
     */
    ESP_ERROR_CHECK(sd_card_mount());
    ESP_ERROR_CHECK(sd_card_test());

    /*
     * TFT second.
     */
    ESP_ERROR_CHECK(display_init());

    // Green screen = hardware ready
    ESP_ERROR_CHECK(display_fill(0x07E0));

    ESP_LOGI(TAG, "Display: OK");
    ESP_LOGI(TAG, "SD card: OK");
    ESP_LOGI(TAG, "Keypad: OK");
    ESP_LOGI(TAG, "Hardware bring-up completed");

    /*
     * Main thread waits for the separate power button.
     * Keypad debug monitor continues in its own FreeRTOS task.
     */
    power_control_wait_for_shutdown_request();

    ESP_LOGW(TAG, "Safe shutdown started");

    /*
     * Stop debug task before shutdown.
     */
    keypad_stop_event_monitor();

    /*
     * Safe SD unmount.
     */
    sd_card_unmount();

    ESP_LOGI(TAG, "SD card unmounted");

    /*
     * Clear TFT.
     */
    esp_err_t display_err = display_fill(0x0000);

    if (display_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to clear display: %s",
            esp_err_to_name(display_err)
        );
    }

    ESP_LOGI(TAG, "Display off");
    ESP_LOGI(TAG, "Cutting power");

    power_control_shutdown();
}