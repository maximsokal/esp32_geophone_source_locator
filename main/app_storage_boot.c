#include "app_storage_boot.h"

#include "app_poweroff.h"

#include "app_config.h"
#include "display.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "ui.h"

#include "esp_err.h"
#include "esp_log.h"


static const char *TAG =
    "APP";


static void app_storage_boot_failure(
    const char *message
)
{
    ESP_LOGE(
        TAG,
        "Storage boot failure: %s",
        message
    );

    bool ui_started =
        false;

    esp_err_t display_err =
        display_init();

    if (display_err == ESP_OK) {
        esp_err_t ui_err =
            ui_boot_init();

        if (ui_err == ESP_OK) {
            ui_started =
                true;

            ui_boot_show_error(
                message
            );
        } else {
            ESP_LOGE(
                TAG,
                "LVGL boot error UI failed: %s",
                esp_err_to_name(ui_err)
            );
        }
    } else {
        ESP_LOGE(
            TAG,
            "Display initialization failed: %s",
            esp_err_to_name(display_err)
        );
    }

    power_control_wait_for_shutdown_request();

    (void)sd_card_unmount();

    if (ui_started) {
        ui_shutdown();
    }

    if (display_err == ESP_OK) {
        (void)display_set_enabled(
            false
        );

        display_deinit();
    }

    power_control_shutdown();

    app_poweroff_idle_forever();
}


void app_storage_boot_run_diagnostic(void)
{
    ESP_LOGI(
        TAG,
        "Starting isolated SD startup check"
    );

    esp_err_t err =
        sd_card_mount();

    if (err != ESP_OK) {
        app_storage_boot_failure(
            "MOUNT FAILED"
        );
    }

#if APP_SD_BOOT_WRITE_TEST_ENABLED
    ESP_LOGI(
        TAG,
        "Boot SD write/read diagnostic enabled"
    );

    err =
        sd_card_diagnostics_run_write_read_test();

    if (err != ESP_OK) {
        app_storage_boot_failure(
            "WRITE TEST FAILED"
        );
    }
#else
    ESP_LOGI(
        TAG,
        "Boot SD write/read diagnostic disabled"
    );
#endif

    ESP_LOGI(
        TAG,
        "Isolated SD startup check passed"
    );
}
