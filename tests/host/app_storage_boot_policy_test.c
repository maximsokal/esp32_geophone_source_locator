#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_storage_boot_policy.h"
#include "esp_err.h"


static app_storage_boot_reason_t expected_reason(app_storage_boot_stage_t stage)
{
    switch (stage) {
        case APP_STORAGE_BOOT_STAGE_HOST_INIT:
            return APP_STORAGE_BOOT_REASON_HOST_INIT_FAILED;
        case APP_STORAGE_BOOT_STAGE_CARD_INIT:
            return APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE;
        case APP_STORAGE_BOOT_STAGE_FS_MOUNT:
            return APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED;
        case APP_STORAGE_BOOT_STAGE_WRITE_PROBE:
            return APP_STORAGE_BOOT_REASON_WRITE_PROBE_FAILED;
        case APP_STORAGE_BOOT_STAGE_LAYOUT_INIT:
            return APP_STORAGE_BOOT_REASON_LAYOUT_INIT_FAILED;
        default:
            return APP_STORAGE_BOOT_REASON_NONE;
    }
}


static app_storage_boot_action_t expected_action(
    app_storage_boot_stage_t stage,
    esp_err_t err,
    esp_err_t probe_result
)
{
    if (stage == APP_STORAGE_BOOT_STAGE_HOST_INIT ||
        stage == APP_STORAGE_BOOT_STAGE_CARD_INIT) {
        return APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY;
    }
    if (stage == APP_STORAGE_BOOT_STAGE_FS_MOUNT &&
        (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE)) {
        return APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY;
    }
    if (stage == APP_STORAGE_BOOT_STAGE_WRITE_PROBE &&
        (probe_result == ESP_ERR_TIMEOUT || probe_result == ESP_ERR_INVALID_RESPONSE)) {
        return APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY;
    }
    return APP_STORAGE_BOOT_ACTION_OFFER_FORMAT;
}


static esp_err_t expected_error(
    app_storage_boot_stage_t stage,
    const app_storage_boot_policy_input_t *input
)
{
    if (stage == APP_STORAGE_BOOT_STAGE_WRITE_PROBE) {
        return input->probe_result;
    }
    if (stage == APP_STORAGE_BOOT_STAGE_LAYOUT_INIT) {
        return input->storage_manager_init_result;
    }
    return input->err;
}


static const char *stage_text(app_storage_boot_stage_t stage)
{
    switch (stage) {
        case APP_STORAGE_BOOT_STAGE_HOST_INIT: return "HOST_INIT";
        case APP_STORAGE_BOOT_STAGE_CARD_INIT: return "CARD_INIT";
        case APP_STORAGE_BOOT_STAGE_FS_MOUNT: return "FS_MOUNT";
        case APP_STORAGE_BOOT_STAGE_WRITE_PROBE: return "WRITE_PROBE";
        case APP_STORAGE_BOOT_STAGE_LAYOUT_INIT: return "LAYOUT_INIT";
        default: return "UNKNOWN";
    }
}


static void test_full_input_product(void)
{
    static const esp_err_t errors[] = {
        ESP_OK,
        ESP_FAIL,
        ESP_ERR_TIMEOUT,
        ESP_ERR_INVALID_RESPONSE,
        ESP_ERR_INVALID_STATE,
    };
    static const int fresults[] = {-1, 13, 2};
    static const app_storage_boot_stage_t stages[] = {
        APP_STORAGE_BOOT_STAGE_HOST_INIT,
        APP_STORAGE_BOOT_STAGE_CARD_INIT,
        APP_STORAGE_BOOT_STAGE_FS_MOUNT,
        APP_STORAGE_BOOT_STAGE_WRITE_PROBE,
        APP_STORAGE_BOOT_STAGE_LAYOUT_INIT,
    };

    unsigned cases = 0U;
    for (size_t stage_index = 0U; stage_index < sizeof(stages) / sizeof(stages[0]); ++stage_index) {
        const app_storage_boot_stage_t stage = stages[stage_index];
        for (size_t err_index = 0U; err_index < sizeof(errors) / sizeof(errors[0]); ++err_index) {
            for (size_t fr_index = 0U; fr_index < sizeof(fresults) / sizeof(fresults[0]); ++fr_index) {
                for (size_t probe_index = 0U; probe_index < sizeof(errors) / sizeof(errors[0]); ++probe_index) {
                    for (size_t storage_index = 0U; storage_index < sizeof(errors) / sizeof(errors[0]); ++storage_index) {
                        const app_storage_boot_policy_input_t input = {
                            .stage = stage,
                            .err = errors[err_index],
                            .fresult = fresults[fr_index],
                            .probe_result = errors[probe_index],
                            .storage_manager_init_result = errors[storage_index],
                        };
                        const app_storage_boot_policy_result_t result =
                            app_storage_boot_policy_classify(&input);
                        assert(result.reason == expected_reason(stage));
                        assert(result.action == expected_action(
                            stage,
                            input.err,
                            input.probe_result
                        ));

                        char expected_text[80];
                        if (stage == APP_STORAGE_BOOT_STAGE_FS_MOUNT && input.fresult >= 0) {
                            (void)snprintf(
                                expected_text,
                                sizeof(expected_text),
                                "%s err=%s fr=%d",
                                stage_text(stage),
                                esp_err_to_name(input.err),
                                input.fresult
                            );
                        } else {
                            (void)snprintf(
                                expected_text,
                                sizeof(expected_text),
                                "%s err=%s",
                                stage_text(stage),
                                esp_err_to_name(expected_error(stage, &input))
                            );
                        }
                        assert(strcmp(result.reason_text_code, expected_text) == 0);
                        ++cases;
                    }
                }
            }
        }
    }
    assert(cases == 1875U);
}


static void test_no_card_and_no_response_never_offer_format(void)
{
    for (int stage = APP_STORAGE_BOOT_STAGE_HOST_INIT;
         stage <= APP_STORAGE_BOOT_STAGE_FS_MOUNT;
         ++stage) {
        const app_storage_boot_policy_input_t input = {
            .stage = (app_storage_boot_stage_t)stage,
            .err = stage == APP_STORAGE_BOOT_STAGE_FS_MOUNT ? ESP_ERR_TIMEOUT : ESP_FAIL,
            .fresult = -1,
            .probe_result = ESP_OK,
            .storage_manager_init_result = ESP_OK,
        };
        const app_storage_boot_policy_result_t result =
            app_storage_boot_policy_classify(&input);
        assert(result.action == APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY);
    }
}


static void test_fr13_esp_fail_offers_format_with_visible_reason(void)
{
    const app_storage_boot_policy_input_t input = {
        .stage = APP_STORAGE_BOOT_STAGE_FS_MOUNT,
        .err = ESP_FAIL,
        .fresult = 13,
        .probe_result = ESP_OK,
        .storage_manager_init_result = ESP_OK,
    };
    const app_storage_boot_policy_result_t result =
        app_storage_boot_policy_classify(&input);
    assert(result.reason == APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED);
    assert(result.action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT);
    assert(strcmp(result.reason_text_code, "FS_MOUNT err=ESP_FAIL fr=13") == 0);
}


static void test_probe_details_are_visible_without_changing_recovery_action(void)
{
    static const char *stages[] = {
        "MOUNT_CHECK", "CREATE", "WRITE", "FLUSH", "CLOSE", "READ", "COMPARE", "REMOVE",
    };
    for (size_t i = 0U; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        const app_storage_boot_policy_input_t input = {
            .stage = APP_STORAGE_BOOT_STAGE_WRITE_PROBE,
            .probe_result = ESP_FAIL,
            .probe_stage_name = stages[i],
            .probe_errno = 28,
        };
        const app_storage_boot_policy_result_t result = app_storage_boot_policy_classify(&input);
        char expected[80];
        (void)snprintf(expected, sizeof(expected), "WRITE_PROBE/%s err=ESP_FAIL errno=28", stages[i]);
        assert(strcmp(result.reason_text_code, expected) == 0);
        assert(result.action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT);
    }
}

int main(void)
{
    test_full_input_product();
    test_no_card_and_no_response_never_offer_format();
    test_fr13_esp_fail_offers_format_with_visible_reason();
    test_probe_details_are_visible_without_changing_recovery_action();
    puts("APP_STORAGE_BOOT_POLICY_HOST_PASS");
    return 0;
}
