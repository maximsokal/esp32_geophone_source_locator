#include "esp_err.h"
#include "esp_log.h"

#include "spi_bus.h"
#include "sd_card.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "object_recovery.h"
#include "object_store.h"
#include "object_service.h"
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "APP";


static void app_idle_after_power_off(void)
{
    /*
     * Pololu removes project power in normal standalone use.
     *
     * During USB debugging the ESP32 can remain electrically
     * powered after Pololu turns the project rail off. Keep
     * app_main alive in a safe idle state instead of returning.
     */
    while (true) {
        vTaskDelay(
            pdMS_TO_TICKS(
                1000
            )
        );
    }
}


static void app_storage_boot_failure(
    const char *message
)
{
    ESP_LOGE(
        TAG,
        "Storage boot failure: %s",
        message
    );

    /*
     * TFT is initialized only after the isolated SD test
     * has already failed.
     */
    esp_err_t display_err =
        display_init();

    if (display_err == ESP_OK) {
        display_fill(
            DISPLAY_COLOR_BLACK
        );

        display_draw_text_fast(
            20,
            68,
            "SD CARD ERROR",
            DISPLAY_COLOR_RED,
            DISPLAY_COLOR_BLACK,
            2
        );

        display_draw_text_fast(
            20,
            108,
            message,
            DISPLAY_COLOR_WHITE,
            DISPLAY_COLOR_BLACK,
            1
        );

        display_draw_text_fast(
            20,
            138,
            "SD TEST: 2 MHZ",
            DISPLAY_COLOR_YELLOW,
            DISPLAY_COLOR_BLACK,
            1
        );

        display_draw_text_fast(
            20,
            164,
            "CHECK CARD AND WIRING",
            DISPLAY_COLOR_WHITE,
            DISPLAY_COLOR_BLACK,
            1
        );

        display_draw_text_fast(
            20,
            190,
            "PRESS POWER TO SHUT DOWN",
            DISPLAY_COLOR_WHITE,
            DISPLAY_COLOR_BLACK,
            1
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "Display initialization failed: %s",
            esp_err_to_name(
                display_err
            )
        );
    }

    /*
     * No reboot loop.
     * Wait for an explicit power-button request.
     */
    power_control_wait_for_shutdown_request();

    sd_card_unmount();

    if (display_err == ESP_OK) {
        display_fill(
            DISPLAY_COLOR_BLACK
        );
    }

    power_control_shutdown();

    app_idle_after_power_off();
}


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
        keypad_init()
    );

    ESP_ERROR_CHECK(
        app_spi_bus_init()
    );

    /*
     * Diagnostic SD phase.
     *
     * Wi-Fi, web services and TFT are intentionally not
     * initialized yet. Only ESP32 + SPI bus + SD are active.
     */
    ESP_LOGI(
        TAG,
        "Starting isolated SD diagnostic"
    );

    esp_err_t sd_mount_err =
        sd_card_mount();

    if (sd_mount_err != ESP_OK) {
        app_storage_boot_failure(
            "MOUNT FAILED"
        );
    }

    esp_err_t sd_test_err =
        sd_card_test();

    if (sd_test_err != ESP_OK) {
        app_storage_boot_failure(
            "WRITE TEST FAILED"
        );
    }

    ESP_LOGI(
        TAG,
        "Isolated SD diagnostic passed"
    );

    /*
     * Only after SD write/read is confirmed do we start
     * the display and higher-current Wi-Fi subsystem.
     */
    ESP_ERROR_CHECK(
        display_init()
    );

    ESP_ERROR_CHECK(
        storage_manager_init()
    );

    ESP_ERROR_CHECK(
        object_recovery_run()
    );

    ESP_ERROR_CHECK(
        object_store_init()
    );

    ESP_ERROR_CHECK(
        object_service_init()
    );

    ESP_ERROR_CHECK(
        storage_service_init()
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
        ui_init()
    );

    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );

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
        "Storage manager: OK"
    );

    ESP_LOGI(
        TAG,
        "Storage service: OK"
    );

    ESP_LOGI(
        TAG,
        "Object recovery: OK"
    );

    ESP_LOGI(
        TAG,
        "Object store: OK"
    );

    ESP_LOGI(
        TAG,
        "Object service: OK"
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

    storage_service_shutdown();

    object_service_shutdown();

    seismic_test_generator_stop();

    esp_err_t portal_err =
        captive_portal_stop();

    if (portal_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop captive portal: %s",
            esp_err_to_name(
                portal_err
            )
        );
    }

    esp_err_t web_err =
        web_server_stop();

    if (web_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop web server: %s",
            esp_err_to_name(
                web_err
            )
        );
    }

    esp_err_t wifi_err =
        wifi_manager_stop();

    if (wifi_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop Wi-Fi: %s",
            esp_err_to_name(
                wifi_err
            )
        );
    }

    esp_err_t storage_err =
        storage_manager_shutdown();

    if (storage_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Storage shutdown failed: %s",
            esp_err_to_name(
                storage_err
            )
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

    app_idle_after_power_off();
}
