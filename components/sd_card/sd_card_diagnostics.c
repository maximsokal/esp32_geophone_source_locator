#include "sd_card_diagnostics.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "sd_card.h"
#include "sd_card_diagnostics_internal.h"


static const char *TAG =
    "SD_CARD";


static const char *TEST_FILE_PATH =
    "/sdcard/.geolocator_sd_test.tmp";


static const char *TEST_TEXT =
    "ESP32 Geophone Source Locator\r\n"
    "SD card read/write test OK\r\n";


static void sd_card_diagnostics_cleanup(void)
{
    if (
        remove(
            TEST_FILE_PATH
        ) != 0
    ) {
        ESP_LOGW(
            TAG,
            "Failed to remove temporary SD test file"
        );
    }
}


esp_err_t sd_card_diagnostics_run_write_read_test(void)
{
    if (!sd_card_is_mounted()) {
        ESP_LOGE(
            TAG,
            "SD card is not mounted"
        );

        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Starting optional SD write/read diagnostic"
    );

    esp_err_t err =
        sd_card_diagnostics_write_file(
            TEST_FILE_PATH,
            TEST_TEXT
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD diagnostic write failed"
        );

        sd_card_diagnostics_cleanup();

        return err;
    }

    char buffer[128] = {0};

    err =
        sd_card_diagnostics_read_file(
            TEST_FILE_PATH,
            buffer,
            sizeof(buffer)
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD diagnostic read failed"
        );

        sd_card_diagnostics_cleanup();

        return err;
    }

    if (
        strcmp(
            buffer,
            TEST_TEXT
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "SD diagnostic read-back verification failed"
        );

        sd_card_diagnostics_cleanup();

        return ESP_FAIL;
    }

    sd_card_diagnostics_cleanup();

    ESP_LOGI(
        TAG,
        "Optional SD write/read diagnostic passed"
    );

    return ESP_OK;
}
