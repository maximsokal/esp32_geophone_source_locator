#include "app_shutdown.h"

#include "app_poweroff.h"

#include "display.h"
#include "keypad.h"
#include "object_service.h"
#include "power_control.h"
#include "sd_card.h"
#include "seismic_acquisition.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_err.h"
#include "esp_log.h"


static const char *TAG =
    "APP";


static void app_shutdown_stop_network(void)
{
    esp_err_t err =
        web_server_stop();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop web server: %s",
            esp_err_to_name(err)
        );
    }

    err =
        wifi_manager_stop();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to stop Wi-Fi: %s",
            esp_err_to_name(err)
        );
    }
}


static void app_shutdown_stop_storage(void)
{
    esp_err_t err =
        storage_manager_shutdown();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Storage shutdown failed: %s",
            esp_err_to_name(err)
        );
    }

    err =
        sd_card_unmount();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "SD card unmount failed: %s",
            esp_err_to_name(err)
        );
    } else {
        ESP_LOGI(
            TAG,
            "SD card unmounted"
        );
    }
}


static void app_shutdown_clear_display(void)
{
    esp_err_t err =
        display_fill(
            DISPLAY_COLOR_BLACK
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to clear display: %s",
            esp_err_to_name(err)
        );
    }

    ESP_LOGI(
        TAG,
        "Display off"
    );
}


void app_shutdown_wait_and_run(void)
{
    power_control_wait_for_shutdown_request();

    ESP_LOGW(
        TAG,
        "Safe shutdown started"
    );

    keypad_stop_event_monitor();

    ui_shutdown();

    storage_service_shutdown();

    object_service_shutdown();

    seismic_acquisition_stop();

    app_shutdown_stop_network();

    app_shutdown_stop_storage();

    app_shutdown_clear_display();

    ESP_LOGI(
        TAG,
        "Cutting power"
    );

    power_control_shutdown();

    app_poweroff_idle_forever();
}
