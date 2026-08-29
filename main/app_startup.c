#include "app_startup.h"

#include "app_startup_status.h"
#include "app_storage_boot.h"

#include <stddef.h>

#include "adc_service.h"
#include "app_event_bus.h"
#include "device_status.h"
#include "display.h"
#include "keypad.h"
#include "object_recovery.h"
#include "object_service.h"
#include "object_store.h"
#include "power_control.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "spi_bus.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_err.h"
#include "esp_log.h"


static const char *TAG =
    "APP";


static void app_startup_initialize_core(void)
{
    ESP_ERROR_CHECK(
        app_event_bus_init()
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
        adc_service_init()
    );

    ESP_ERROR_CHECK(
        keypad_init()
    );

    ESP_ERROR_CHECK(
        app_spi_bus_init()
    );
}


static void app_startup_initialize_services(void)
{
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
        ui_init()
    );

    ESP_ERROR_CHECK(
        keypad_start_event_monitor(
            ui_handle_keypad_event,
            NULL
        )
    );

    /*
     * Acquisition hardware is allowed to be absent/faulty
     * during hardware bring-up.
     *
     * A failed ADC must not reboot the entire controller.
     */
    const esp_err_t acquisition_err =
        seismic_acquisition_start();

    if (acquisition_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Seismic acquisition unavailable: %s",
            esp_err_to_name(
                acquisition_err
            )
        );

        /*
         * Explicitly expose a stopped acquisition state.
         */
        esp_err_t status_err =
            device_status_set_acquisition(
                false,
                0,
                0,
                0
            );

        if (status_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Unable to update acquisition status: %s",
                esp_err_to_name(
                    status_err
                )
            );
        }
    }
}


void app_startup_run(void)
{
    app_startup_initialize_core();

    app_storage_boot_run_diagnostic();

    app_startup_initialize_services();

    app_startup_log_success();
}