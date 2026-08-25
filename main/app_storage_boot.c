#include "app_storage_boot.h"

#include "app_poweroff.h"

#include "app_config.h"
#include "display.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"

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
            "SD SPI: 2 MHZ",
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

    power_control_wait_for_shutdown_request();

    sd_card_unmount();

    if (display_err == ESP_OK) {
        display_fill(
            DISPLAY_COLOR_BLACK
        );
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
