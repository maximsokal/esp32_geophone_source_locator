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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "APP";


/*
 * Tolerates a card that needs a moment longer than the first probe allows.
 * This is insurance against a slow/transient power-up only: a card that is
 * absent, unpowered or miswired fails all attempts identically, so repeated
 * failures here mean the fault is physical, not a boot-timing race.
 */
#define APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS   3
#define APP_STORAGE_BOOT_SD_MOUNT_RETRY_MS   150

/*
 * A 250 ms delay before the first attempt was tried here on the theory
 * that the shared rail needed more time to settle before the first SD
 * probe. It made brownouts worse, not better -- disproving that theory --
 * so it was reverted. The retry loop below is the only mitigation that
 * has ever measurably helped (a genuinely slow card that answers on a
 * later attempt), and even that has not been observed to change the
 * outcome on this specific hardware. Do not reintroduce a blind delay
 * without a specific, tested mechanism.
 */
static esp_err_t app_storage_boot_mount_sd_with_retry(void)
{
    esp_err_t err =
        ESP_FAIL;

    for (
        int attempt = 1;
        attempt <= APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS;
        ++attempt
    ) {
        err =
            sd_card_mount();

        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGW(
                    TAG,
                    "SD mount succeeded on attempt %d/%d",
                    attempt,
                    APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS
                );
            }

            return ESP_OK;
        }

        if (attempt < APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS) {
            ESP_LOGW(
                TAG,
                "SD mount attempt %d/%d failed (%s); retrying after %d ms settle delay",
                attempt,
                APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS,
                esp_err_to_name(err),
                APP_STORAGE_BOOT_SD_MOUNT_RETRY_MS
            );

            vTaskDelay(
                pdMS_TO_TICKS(
                    APP_STORAGE_BOOT_SD_MOUNT_RETRY_MS
                )
            );
        }
    }

    return err;
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

    const esp_err_t arm_err =
        power_control_arm_shutdown_button();

    if (arm_err == ESP_OK) {
        power_control_wait_for_shutdown_request();
    } else {
        ESP_LOGE(
            TAG,
            "Boot-error shutdown button arm failed: %s; skipping unreachable wait and powering off safely",
            esp_err_to_name(arm_err)
        );
    }

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
        app_storage_boot_mount_sd_with_retry();

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
