#include "app_startup.h"

#include "app_startup_status.h"
#include "app_storage_boot.h"

#include <stddef.h>

#include "app_event_bus.h"
#include "device_status.h"
#include "display.h"
#include "keypad.h"
#include "object_recovery.h"
#include "object_service.h"
#include "object_store.h"
#include "power_control.h"
#include "seismic_buffer.h"
#include "seismic_test_generator.h"
#include "spi_bus.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_err.h"


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

    ESP_ERROR_CHECK(
        seismic_test_generator_start()
    );
}


void app_startup_run(void)
{
    app_startup_initialize_core();

    app_storage_boot_run_diagnostic();

    app_startup_initialize_services();

    app_startup_log_success();
}
