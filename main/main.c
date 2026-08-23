#include "esp_err.h"
#include "esp_log.h"

#include "spi_bus.h"
#include "sd_card.h"
#include "display.h"
#include "power_control.h"
#include "keypad.h"
#include "device_status.h"
#include "ui.h"


static const char *TAG = "APP";


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


    /*
     * From this point every debounced keypad press is
     * delivered to the UI router.
     */
    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );


    ESP_LOGI(TAG, "Display: OK");
    ESP_LOGI(TAG, "SD card: OK");
    ESP_LOGI(TAG, "Keypad: OK");
    ESP_LOGI(TAG, "UI: OK");

    ESP_LOGI(
        TAG,
        "Hardware bring-up completed"
    );


    /*
     * The main task remains dedicated to safe power shutdown.
     */
    power_control_wait_for_shutdown_request();


    ESP_LOGW(
        TAG,
        "Safe shutdown started"
    );


    /*
     * Stop all UI activity before touching the SD card
     * or clearing the display.
     */
    keypad_stop_event_monitor();


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
            esp_err_to_name(
                display_err
            )
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