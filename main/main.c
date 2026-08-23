#include "esp_err.h"
#include "esp_log.h"

#include "spi_bus.h"
#include "sd_card.h"
#include "display.h"
#include "power_control.h"
#include "keypad.h"
#include "device_status.h"
#include "seismic_buffer.h"
#include "seismic_test_generator.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "captive_portal.h"
#include "ui.h"


static const char *TAG =
    "APP";


void app_main(void)
{
    ESP_LOGI(
        TAG,
        "ESP32 Geophone Source Locator"
    );


    ESP_ERROR_CHECK(
        power_control_init()
    );


    ESP_ERROR_CHECK(
        device_status_init()
    );


    ESP_ERROR_CHECK(
        seismic_buffer_init()
    );


    ESP_ERROR_CHECK(
        wifi_manager_init()
    );


    ESP_ERROR_CHECK(
        wifi_manager_start()
    );


    ESP_ERROR_CHECK(
        web_server_start()
    );


    ESP_ERROR_CHECK(
        captive_portal_start()
    );


    ESP_ERROR_CHECK(
        keypad_init()
    );


    ESP_ERROR_CHECK(
        app_spi_bus_init()
    );


    /*
     * The SD card and TFT share SPI2.
     * Initialize the SD card before TFT traffic begins.
     */
    ESP_ERROR_CHECK(
        sd_card_mount()
    );


    ESP_ERROR_CHECK(
        sd_card_test()
    );


    ESP_ERROR_CHECK(
        display_init()
    );


    ESP_ERROR_CHECK(
        ui_init()
    );


    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );


    /*
     * Temporary waveform source used only to verify
     * the seismic web pipeline before AD7606 is connected.
     */
    ESP_ERROR_CHECK(
        seismic_test_generator_start()
    );


    ESP_LOGI(
        TAG,
        "Display: OK"
    );

    ESP_LOGI(
        TAG,
        "SD card: OK"
    );

    ESP_LOGI(
        TAG,
        "Keypad: OK"
    );

    ESP_LOGI(
        TAG,
        "Wi-Fi: OK"
    );

    ESP_LOGI(
        TAG,
        "Web server: OK"
    );

    ESP_LOGI(
        TAG,
        "Captive portal: OK"
    );

    ESP_LOGI(
        TAG,
        "Seismic test pipeline: OK"
    );

    ESP_LOGI(
        TAG,
        "UI: OK"
    );


    ESP_LOGI(
        TAG,
        "Hardware bring-up completed"
    );


    power_control_wait_for_shutdown_request();


    ESP_LOGW(
        TAG,
        "Safe shutdown started"
    );


    keypad_stop_event_monitor();


    seismic_test_generator_stop();


    esp_err_t portal_err =
        captive_portal_stop();


    if (portal_err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Failed to stop captive portal: %s",
            esp_err_to_name(portal_err)
        );
    }


    esp_err_t web_err =
        web_server_stop();


    if (web_err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Failed to stop web server: %s",
            esp_err_to_name(web_err)
        );
    }


    esp_err_t wifi_err =
        wifi_manager_stop();


    if (wifi_err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Failed to stop Wi-Fi: %s",
            esp_err_to_name(wifi_err)
        );
    }


    sd_card_unmount();


    ESP_LOGI(
        TAG,
        "SD card unmounted"
    );


    esp_err_t display_err =
        display_fill(
            DISPLAY_COLOR_BLACK
        );


    if (display_err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Failed to clear display: %s",
            esp_err_to_name(display_err)
        );
    }


    ESP_LOGI(
        TAG,
        "Display off"
    );


    ESP_LOGI(
        TAG,
        "Cutting power"
    );


    power_control_shutdown();
}