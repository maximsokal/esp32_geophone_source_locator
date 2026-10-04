#include "app_storage_boot_policy.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_err.h"


static bool app_storage_boot_is_no_response(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE;
}


app_storage_boot_policy_result_t app_storage_boot_policy_classify(
    const app_storage_boot_policy_input_t *input
)
{
    app_storage_boot_policy_result_t result = {
        .reason = APP_STORAGE_BOOT_REASON_NONE,
        .action = APP_STORAGE_BOOT_ACTION_NONE,
        .reason_text_code = "UNKNOWN",
    };

    if (input == NULL) {
        return result;
    }

    const char *stage_name = "UNKNOWN";
    esp_err_t relevant_err = input->err;

    switch (input->stage) {
        case APP_STORAGE_BOOT_STAGE_HOST_INIT:
            result.reason = APP_STORAGE_BOOT_REASON_HOST_INIT_FAILED;
            result.action = APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY;
            stage_name = "HOST_INIT";
            break;

        case APP_STORAGE_BOOT_STAGE_CARD_INIT:
            result.reason = APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE;
            result.action = APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY;
            stage_name = "CARD_INIT";
            break;

        case APP_STORAGE_BOOT_STAGE_FS_MOUNT:
            result.reason = APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED;
            result.action = app_storage_boot_is_no_response(input->err)
                ? APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY
                : APP_STORAGE_BOOT_ACTION_OFFER_FORMAT;
            stage_name = "FS_MOUNT";
            break;

        case APP_STORAGE_BOOT_STAGE_WRITE_PROBE:
            result.reason = APP_STORAGE_BOOT_REASON_WRITE_PROBE_FAILED;
            relevant_err = input->probe_result;
            result.action = app_storage_boot_is_no_response(relevant_err)
                ? APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY
                : APP_STORAGE_BOOT_ACTION_OFFER_FORMAT;
            stage_name = "WRITE_PROBE";
            break;

        case APP_STORAGE_BOOT_STAGE_LAYOUT_INIT:
            result.reason = APP_STORAGE_BOOT_REASON_LAYOUT_INIT_FAILED;
            relevant_err = input->storage_manager_init_result;
            result.action = APP_STORAGE_BOOT_ACTION_OFFER_FORMAT;
            stage_name = "LAYOUT_INIT";
            break;

        default:
            return result;
    }

    if (input->stage == APP_STORAGE_BOOT_STAGE_WRITE_PROBE && input->probe_stage_name != NULL) {
        (void)snprintf(
            result.reason_text_code,
            sizeof(result.reason_text_code),
            "%s/%s err=%s errno=%d",
            stage_name,
            input->probe_stage_name,
            esp_err_to_name(relevant_err),
            input->probe_errno
        );
    } else if (input->stage == APP_STORAGE_BOOT_STAGE_FS_MOUNT && input->fresult >= 0) {
        (void)snprintf(
            result.reason_text_code,
            sizeof(result.reason_text_code),
            "%s err=%s fr=%d",
            stage_name,
            esp_err_to_name(relevant_err),
            input->fresult
        );
    } else {
        (void)snprintf(
            result.reason_text_code,
            sizeof(result.reason_text_code),
            "%s err=%s",
            stage_name,
            esp_err_to_name(relevant_err)
        );
    }

    return result;
}
