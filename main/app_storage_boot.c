#include "app_storage_boot.h"

#include <string.h>

#include "app_poweroff.h"
#include "app_storage_boot_ui.h"

#include "app_config.h"
#include "display.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "sd_card_format.h"
#include "storage_manager.h"
#include "ui.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "APP";

static app_storage_recovery_ui_t s_recovery_ui;


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
static esp_err_t app_storage_boot_mount_sd_with_retry(
    sd_card_mount_report_t *out_report
)
{
    esp_err_t err =
        ESP_FAIL;
    sd_card_mount_report_t report = {
        .stage = SD_CARD_MOUNT_STAGE_NONE,
        .err = ESP_FAIL,
        .fresult = -1,
        .health = SD_CARD_HEALTH_UNKNOWN,
    };

    for (
        int attempt = 1;
        attempt <= APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS;
        ++attempt
    ) {
        err = sd_card_mount_recovery(&report);

        if (err == ESP_OK) {
            if (out_report != NULL) {
                *out_report = report;
            }
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
                "SD mount attempt %d/%d failed at stage=%d (%s, fr=%d); retrying after %d ms settle delay",
                attempt,
                APP_STORAGE_BOOT_SD_MOUNT_ATTEMPTS,
                (int)report.stage,
                esp_err_to_name(report.err),
                report.fresult,
                APP_STORAGE_BOOT_SD_MOUNT_RETRY_MS
            );

            vTaskDelay(
                pdMS_TO_TICKS(
                    APP_STORAGE_BOOT_SD_MOUNT_RETRY_MS
                )
            );
        }
    }

    if (out_report != NULL) {
        *out_report = report;
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

    const esp_err_t ui_err = app_storage_boot_ui_ensure_initialized();
    const bool ui_started = ui_err == ESP_OK;

    if (ui_started && !app_storage_boot_ui_has_recovery_view()) {
        ui_boot_show_error(message);
    } else if (ui_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Boot recovery display/UI initialization failed: %s",
            esp_err_to_name(ui_err)
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

    if (display_is_ready()) {
        (void)display_set_enabled(
            false
        );

        display_deinit();
    }

    power_control_shutdown();

    app_poweroff_idle_forever();
}


void app_storage_boot_set_recovery_ui(const app_storage_recovery_ui_t *ui)
{
    if (ui == NULL) {
        memset(&s_recovery_ui, 0, sizeof(s_recovery_ui));
        return;
    }
    s_recovery_ui = *ui;
}


static app_storage_boot_stage_t app_storage_boot_policy_stage_from_mount(
    sd_card_mount_stage_t stage
)
{
    switch (stage) {
        case SD_CARD_MOUNT_STAGE_HOST_INIT:
            return APP_STORAGE_BOOT_STAGE_HOST_INIT;
        case SD_CARD_MOUNT_STAGE_CARD_INIT:
            return APP_STORAGE_BOOT_STAGE_CARD_INIT;
        case SD_CARD_MOUNT_STAGE_FS_MOUNT:
        case SD_CARD_MOUNT_STAGE_NONE:
        default:
            return APP_STORAGE_BOOT_STAGE_FS_MOUNT;
    }
}


static bool app_storage_boot_recover(
    app_storage_boot_policy_input_t input
)
{
    if (s_recovery_ui.show_reason == NULL || s_recovery_ui.wait_decision == NULL) {
        return false;
    }

    for (;;) {
        const app_storage_boot_policy_result_t policy =
            app_storage_boot_policy_classify(&input);
        if (policy.action == APP_STORAGE_BOOT_ACTION_NONE) {
            return false;
        }
        s_recovery_ui.show_reason(s_recovery_ui.context, &policy);
        const app_storage_recovery_decision_t decision =
            s_recovery_ui.wait_decision(s_recovery_ui.context, policy.action);

        if (decision == APP_STORAGE_RECOVERY_DECISION_CANCEL) {
            return false;
        }
        if (decision == APP_STORAGE_RECOVERY_DECISION_FORMAT) {
            if (policy.action != APP_STORAGE_BOOT_ACTION_OFFER_FORMAT) {
                return false;
            }

            esp_err_t err = sd_card_format_fat32_task_start(
                sd_card_get_card_handle(),
                true
            );
            if (err != ESP_OK) {
                if (s_recovery_ui.show_result != NULL) {
                    s_recovery_ui.show_result(s_recovery_ui.context, false, err);
                }
                continue;
            }

            sd_card_format_status_t status = {
                .state = SD_CARD_FORMAT_STATE_RUNNING,
                .percent = 0U,
                .err = ESP_OK,
                .fresult = -1,
            };
            while (status.state == SD_CARD_FORMAT_STATE_RUNNING) {
                if (s_recovery_ui.show_progress != NULL) {
                    s_recovery_ui.show_progress(s_recovery_ui.context, status.percent);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                err = sd_card_format_fat32_task_get_status(&status);
                if (err != ESP_OK) {
                    status.state = SD_CARD_FORMAT_STATE_FAILED;
                    status.err = err;
                }
            }

            const bool success = status.state == SD_CARD_FORMAT_STATE_DONE;
            if (s_recovery_ui.show_progress != NULL && success) {
                s_recovery_ui.show_progress(s_recovery_ui.context, 100U);
            }
            if (s_recovery_ui.show_result != NULL) {
                s_recovery_ui.show_result(
                    s_recovery_ui.context,
                    success,
                    status.err
                );
            }
            if (success) {
                return true;
            }
            continue;
        }
        if (decision != APP_STORAGE_RECOVERY_DECISION_RETRY) {
            return false;
        }

        sd_card_mount_report_t mount_report = {
            .stage = SD_CARD_MOUNT_STAGE_NONE,
            .err = ESP_OK,
            .fresult = -1,
            .health = SD_CARD_HEALTH_UNKNOWN,
        };
        const esp_err_t mount_err = app_storage_boot_mount_sd_with_retry(&mount_report);
        if (mount_err != ESP_OK) {
            input.stage = app_storage_boot_policy_stage_from_mount(mount_report.stage);
            input.err = mount_report.err;
            input.fresult = mount_report.fresult;
            input.probe_result = ESP_OK;
            continue;
        }

        const sd_card_probe_result_t probe = sd_card_write_readback_probe();
        if (probe.err == ESP_OK) {
            if (s_recovery_ui.show_result != NULL) {
                s_recovery_ui.show_result(s_recovery_ui.context, true, ESP_OK);
            }
            return true;
        }
        input.stage = APP_STORAGE_BOOT_STAGE_WRITE_PROBE;
        input.err = ESP_OK;
        input.fresult = -1;
        input.probe_result = probe.err;
        input.probe_stage_name = sd_card_probe_stage_name(probe.stage);
        input.probe_errno = probe.sys_errno;
    }
}


void app_storage_boot_run_diagnostic(void)
{
    ESP_LOGI(
        TAG,
        "Starting isolated SD startup check"
    );

    sd_card_mount_report_t mount_report = {
        .stage = SD_CARD_MOUNT_STAGE_NONE,
        .err = ESP_FAIL,
        .fresult = -1,
        .health = SD_CARD_HEALTH_UNKNOWN,
    };
    esp_err_t err = app_storage_boot_mount_sd_with_retry(&mount_report);
    bool storage_ready = false;

    if (err != ESP_OK) {
        app_storage_boot_policy_input_t input = {
            .stage = app_storage_boot_policy_stage_from_mount(mount_report.stage),
            .err = mount_report.err,
            .fresult = mount_report.fresult,
            .probe_result = ESP_OK,
            .storage_manager_init_result = ESP_OK,
        };
        if (!app_storage_boot_recover(input)) {
            app_storage_boot_failure("MOUNT FAILED");
        }
        storage_ready = true;
    }

    if (!storage_ready) {
        const sd_card_probe_result_t probe = sd_card_write_readback_probe();
        if (probe.err != ESP_OK) {
            const app_storage_boot_policy_input_t input = {
                .stage = APP_STORAGE_BOOT_STAGE_WRITE_PROBE,
                .err = ESP_OK,
                .fresult = -1,
                .probe_result = probe.err,
                .probe_stage_name = sd_card_probe_stage_name(probe.stage),
                .probe_errno = probe.sys_errno,
                .storage_manager_init_result = ESP_OK,
            };
            if (!app_storage_boot_recover(input)) {
                app_storage_boot_failure("WRITE TEST FAILED");
            }
        }
    }

#if APP_SD_BOOT_WRITE_TEST_ENABLED
    ESP_LOGI(
        TAG,
        "Boot SD write/read diagnostic enabled"
    );

    err =
        sd_card_diagnostics_run_write_read_test();

    if (err != ESP_OK) {
        const app_storage_boot_policy_input_t input = {
            .stage = APP_STORAGE_BOOT_STAGE_WRITE_PROBE,
            .err = ESP_OK,
            .fresult = -1,
            .probe_result = err,
            .storage_manager_init_result = ESP_OK,
        };
        if (!app_storage_boot_recover(input)) {
            app_storage_boot_failure("WRITE TEST FAILED");
        }
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


void app_storage_boot_recover_storage_manager_failure(esp_err_t err)
{
    const app_storage_boot_policy_input_t input = {
        .stage = APP_STORAGE_BOOT_STAGE_LAYOUT_INIT,
        .err = ESP_OK,
        .fresult = -1,
        .probe_result = ESP_OK,
        .storage_manager_init_result = err,
    };

    if (!app_storage_boot_recover(input)) {
        app_storage_boot_failure("MOUNT FAILED");
    }

    const esp_err_t retry_err = storage_manager_init();
    if (retry_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Storage manager initialization still failed after recovery: %s",
            esp_err_to_name(retry_err)
        );
        app_storage_boot_failure("MOUNT FAILED");
    }
}
