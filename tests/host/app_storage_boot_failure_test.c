#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_storage_boot.h"
#include "display.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "sd_card_format.h"
#include "storage_manager.h"
#include "ui.h"


typedef enum {
    EVENT_SD_MOUNT = 1,
    EVENT_SD_PROBE,
    EVENT_SD_DIAGNOSTIC,
    EVENT_RECOVERY_REASON,
    EVENT_RECOVERY_DECISION,
    EVENT_FORMAT_START,
    EVENT_FORMAT_STATUS,
    EVENT_RECOVERY_PROGRESS,
    EVENT_RECOVERY_RESULT,
    EVENT_STORAGE_INIT,
    EVENT_DISPLAY_INIT,
    EVENT_UI_INIT,
    EVENT_SHOW_ERROR,
    EVENT_ARM,
    EVENT_WAIT,
    EVENT_UNMOUNT,
    EVENT_UI_SHUTDOWN,
    EVENT_DISPLAY_DISABLE,
    EVENT_DISPLAY_DEINIT,
    EVENT_POWER_SHUTDOWN,
    EVENT_IDLE,
} test_event_t;

static test_event_t s_events[32];
static size_t s_event_count;
static esp_err_t s_mount_result;
static esp_err_t s_mount_script[8];
static size_t s_mount_script_count;
static size_t s_mount_attempts;
static unsigned s_delay_calls;
static TickType_t s_delay_ticks[8];
static esp_err_t s_diagnostic_result;
static esp_err_t s_probe_result;
static esp_err_t s_display_result;
static esp_err_t s_ui_result;
static bool s_display_ready;
static esp_err_t s_arm_result;
static char s_error_message[64];
static jmp_buf s_idle_jump;
static bool s_jump_from_idle;
static sdmmc_card_t s_fake_card;
static app_storage_recovery_decision_t s_decision;
static app_storage_recovery_decision_t s_decision_script[4];
static size_t s_decision_script_count;
static size_t s_decision_script_index;
static app_storage_boot_policy_result_t s_seen_reason;
static app_storage_boot_action_t s_seen_action;
static unsigned s_reason_calls;
static unsigned s_decision_calls;
static unsigned s_format_start_calls;
static unsigned s_format_status_calls;
static unsigned s_show_result_calls;
static bool s_recovery_view_active;
static bool s_last_result_ok;
static esp_err_t s_last_result_err;
static esp_err_t s_format_start_result;
static sd_card_format_status_t s_format_status;
static esp_err_t s_storage_init_result;
static unsigned s_storage_init_calls;

static void push_event(test_event_t event)
{
    assert(s_event_count < sizeof(s_events) / sizeof(s_events[0]));
    s_events[s_event_count++] = event;
}

static void reset_fakes(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0U;
    s_mount_result = ESP_OK;
    memset(s_mount_script, 0, sizeof(s_mount_script));
    s_mount_script_count = 0U;
    s_mount_attempts = 0U;
    s_delay_calls = 0U;
    memset(s_delay_ticks, 0, sizeof(s_delay_ticks));
    s_diagnostic_result = ESP_OK;
    s_probe_result = ESP_OK;
    s_display_result = ESP_OK;
    s_display_ready = false;
    s_ui_result = ESP_OK;
    s_arm_result = ESP_OK;
    s_error_message[0] = '\0';
    s_jump_from_idle = false;
    s_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    memset(s_decision_script, 0, sizeof(s_decision_script));
    s_decision_script_count = 0U;
    s_decision_script_index = 0U;
    memset(&s_seen_reason, 0, sizeof(s_seen_reason));
    s_seen_action = APP_STORAGE_BOOT_ACTION_NONE;
    s_reason_calls = 0U;
    s_decision_calls = 0U;
    s_format_start_calls = 0U;
    s_format_status_calls = 0U;
    s_show_result_calls = 0U;
    s_recovery_view_active = false;
    s_last_result_ok = false;
    s_last_result_err = ESP_OK;
    s_format_start_result = ESP_OK;
    s_format_status = (sd_card_format_status_t){
        .state = SD_CARD_FORMAT_STATE_DONE,
        .percent = 100U,
        .err = ESP_OK,
        .fresult = 0,
    };
    s_storage_init_result = ESP_OK;
    s_storage_init_calls = 0U;
    app_storage_boot_set_recovery_ui(NULL);
}

static void set_mount_script(const esp_err_t *results, size_t count)
{
    assert(count <= sizeof(s_mount_script) / sizeof(s_mount_script[0]));
    memcpy(s_mount_script, results, count * sizeof(s_mount_script[0]));
    s_mount_script_count = count;
}

esp_err_t sd_card_mount_recovery(sd_card_mount_report_t *report)
{
    push_event(EVENT_SD_MOUNT);
    const esp_err_t result =
        s_mount_attempts < s_mount_script_count
            ? s_mount_script[s_mount_attempts]
            : s_mount_result;
    ++s_mount_attempts;
    if (report != NULL) {
        report->stage = result == ESP_OK
            ? SD_CARD_MOUNT_STAGE_NONE
            : (result == ESP_ERR_TIMEOUT || result == ESP_ERR_INVALID_RESPONSE
                ? SD_CARD_MOUNT_STAGE_CARD_INIT
                : SD_CARD_MOUNT_STAGE_FS_MOUNT);
        report->err = result;
        report->fresult = result == ESP_FAIL ? 13 : -1;
        report->health = result == ESP_OK
            ? SD_CARD_HEALTH_READY
            : (report->stage == SD_CARD_MOUNT_STAGE_FS_MOUNT
                ? SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE
                : SD_CARD_HEALTH_CARD_UNAVAILABLE);
    }
    return result;
}

sdmmc_card_t *sd_card_get_card_handle(void)
{
    return &s_fake_card;
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_delay_calls < sizeof(s_delay_ticks) / sizeof(s_delay_ticks[0]));
    s_delay_ticks[s_delay_calls++] = ticks;
}

esp_err_t sd_card_unmount(void)
{
    push_event(EVENT_UNMOUNT);
    return ESP_OK;
}

esp_err_t sd_card_diagnostics_run_write_read_test(void)
{
    push_event(EVENT_SD_DIAGNOSTIC);
    return s_diagnostic_result;
}

sd_card_probe_result_t sd_card_write_readback_probe(void)
{
    push_event(EVENT_SD_PROBE);
    return (sd_card_probe_result_t){
        .stage = s_probe_result == ESP_OK
            ? SD_CARD_PROBE_STAGE_COMPLETE
            : SD_CARD_PROBE_STAGE_WRITE,
        .err = s_probe_result,
        .sys_errno = s_probe_result == ESP_OK ? 0 : 28,
    };
}

const char *sd_card_probe_stage_name(sd_card_probe_stage_t stage)
{
    return stage == SD_CARD_PROBE_STAGE_COMPLETE ? "COMPLETE" : "WRITE";
}

esp_err_t sd_card_format_fat32_task_start(sdmmc_card_t *card, bool confirmed)
{
    push_event(EVENT_FORMAT_START);
    ++s_format_start_calls;
    assert(card == &s_fake_card);
    assert(confirmed);
    return s_format_start_result;
}

esp_err_t sd_card_format_fat32_task_get_status(sd_card_format_status_t *out_status)
{
    push_event(EVENT_FORMAT_STATUS);
    ++s_format_status_calls;
    assert(out_status != NULL);
    *out_status = s_format_status;
    return ESP_OK;
}

esp_err_t sd_card_format_fat32_task_stop(void)
{
    return ESP_OK;
}

esp_err_t storage_manager_init(void)
{
    push_event(EVENT_STORAGE_INIT);
    ++s_storage_init_calls;
    return s_storage_init_result;
}

static void fake_show_reason(void *context, const app_storage_boot_policy_result_t *reason)
{
    (void)context;
    push_event(EVENT_RECOVERY_REASON);
    ++s_reason_calls;
    assert(reason != NULL);
    s_seen_reason = *reason;
    s_recovery_view_active = true;
}

static app_storage_recovery_decision_t fake_wait_decision(
    void *context,
    app_storage_boot_action_t action
)
{
    (void)context;
    push_event(EVENT_RECOVERY_DECISION);
    ++s_decision_calls;
    s_seen_action = action;
    if (s_decision_script_index < s_decision_script_count) {
        return s_decision_script[s_decision_script_index++];
    }
    return s_decision;
}

static void fake_show_progress(void *context, unsigned percent)
{
    (void)context;
    (void)percent;
    push_event(EVENT_RECOVERY_PROGRESS);
}

static void fake_show_result(void *context, bool success, esp_err_t err)
{
    (void)context;
    push_event(EVENT_RECOVERY_RESULT);
    ++s_show_result_calls;
    s_last_result_ok = success;
    s_last_result_err = err;
    s_recovery_view_active = true;
}

static void install_recovery_ui(void)
{
    const app_storage_recovery_ui_t ui = {
        .context = NULL,
        .show_reason = fake_show_reason,
        .wait_decision = fake_wait_decision,
        .show_progress = fake_show_progress,
        .show_result = fake_show_result,
    };
    app_storage_boot_set_recovery_ui(&ui);
}

esp_err_t display_init(void)
{
    push_event(EVENT_DISPLAY_INIT);
    s_display_ready = s_display_result == ESP_OK;
    return s_display_result;
}

bool display_is_ready(void)
{
    return s_display_ready;
}

esp_err_t display_set_enabled(bool enabled)
{
    assert(!enabled);
    push_event(EVENT_DISPLAY_DISABLE);
    return ESP_OK;
}

void display_deinit(void)
{
    push_event(EVENT_DISPLAY_DEINIT);
}

esp_err_t ui_boot_init(void)
{
    push_event(EVENT_UI_INIT);
    return s_ui_result;
}

void ui_boot_show_error(const char *message)
{
    push_event(EVENT_SHOW_ERROR);
    snprintf(s_error_message, sizeof(s_error_message), "%s", message);
}

void ui_shutdown(void)
{
    push_event(EVENT_UI_SHUTDOWN);
}

esp_err_t app_storage_boot_ui_ensure_initialized(void)
{
    const esp_err_t display_err = display_init();
    if (display_err != ESP_OK) {
        return display_err;
    }
    return ui_boot_init();
}

bool app_storage_boot_ui_has_recovery_view(void)
{
    return s_recovery_view_active;
}

esp_err_t power_control_arm_shutdown_button(void)
{
    push_event(EVENT_ARM);
    return s_arm_result;
}

void power_control_wait_for_shutdown_request(void)
{
    push_event(EVENT_WAIT);
}

void power_control_shutdown(void)
{
    push_event(EVENT_POWER_SHUTDOWN);
}

void app_poweroff_idle_forever(void)
{
    push_event(EVENT_IDLE);
    if (s_jump_from_idle) {
        longjmp(s_idle_jump, 1);
    }
}

#include "../../main/app_storage_boot.c"

static void assert_sequence(const test_event_t *expected, size_t count)
{
    assert(s_event_count == count);
    for (size_t i = 0U; i < count; ++i) {
        assert(s_events[i] == expected[i]);
    }
}

static void run_failure_helper(const char *message)
{
    s_jump_from_idle = true;
    if (setjmp(s_idle_jump) == 0) {
        app_storage_boot_failure(message);
        assert(false);
    }
    s_jump_from_idle = false;
}

static void run_diagnostic_until_poweroff(void)
{
    s_jump_from_idle = true;
    if (setjmp(s_idle_jump) == 0) {
        app_storage_boot_run_diagnostic();
        assert(false);
    }
    s_jump_from_idle = false;
}

static void test_arm_success_waits_before_cleanup(void)
{
    reset_fakes();
    run_failure_helper("MOUNT FAILED");

    static const test_event_t expected[] = {
        EVENT_DISPLAY_INIT,
        EVENT_UI_INIT,
        EVENT_SHOW_ERROR,
        EVENT_ARM,
        EVENT_WAIT,
        EVENT_UNMOUNT,
        EVENT_UI_SHUTDOWN,
        EVENT_DISPLAY_DISABLE,
        EVENT_DISPLAY_DEINIT,
        EVENT_POWER_SHUTDOWN,
        EVENT_IDLE,
    };
    assert_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    assert(strcmp(s_error_message, "MOUNT FAILED") == 0);
}

static void test_arm_failure_skips_unreachable_wait_but_keeps_cleanup(void)
{
    reset_fakes();
    s_arm_result = ESP_ERR_INVALID_STATE;
    run_failure_helper("MOUNT FAILED");

    static const test_event_t expected[] = {
        EVENT_DISPLAY_INIT,
        EVENT_UI_INIT,
        EVENT_SHOW_ERROR,
        EVENT_ARM,
        EVENT_UNMOUNT,
        EVENT_UI_SHUTDOWN,
        EVENT_DISPLAY_DISABLE,
        EVENT_DISPLAY_DEINIT,
        EVENT_POWER_SHUTDOWN,
        EVENT_IDLE,
    };
    assert_sequence(expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_display_failure_still_arms_and_waits(void)
{
    reset_fakes();
    s_display_result = ESP_FAIL;
    run_failure_helper("MOUNT FAILED");

    static const test_event_t expected[] = {
        EVENT_DISPLAY_INIT,
        EVENT_ARM,
        EVENT_WAIT,
        EVENT_UNMOUNT,
        EVENT_POWER_SHUTDOWN,
        EVENT_IDLE,
    };
    assert_sequence(expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_mount_failure_uses_same_safe_shutdown_helper(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    run_diagnostic_until_poweroff();

    assert(s_mount_attempts == 3U);
    assert(s_delay_calls == 2U);
    assert(s_delay_ticks[0] == 150U);
    assert(s_delay_ticks[1] == 150U);
    assert(s_events[0] == EVENT_SD_MOUNT);
    assert(s_events[1] == EVENT_SD_MOUNT);
    assert(s_events[2] == EVENT_SD_MOUNT);
    assert(s_events[3] == EVENT_DISPLAY_INIT);
    assert(strcmp(s_error_message, "MOUNT FAILED") == 0);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
}

static void test_write_failure_uses_same_safe_shutdown_helper(void)
{
    reset_fakes();
    s_diagnostic_result = ESP_FAIL;
    run_diagnostic_until_poweroff();

    assert(s_events[0] == EVENT_SD_MOUNT);
    assert(s_events[1] == EVENT_SD_PROBE);
    assert(s_events[2] == EVENT_SD_DIAGNOSTIC);
    assert(s_events[3] == EVENT_DISPLAY_INIT);
    assert(strcmp(s_error_message, "WRITE TEST FAILED") == 0);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
    assert(s_mount_attempts == 1U);
    assert(s_delay_calls == 0U);
}

static void test_mount_retry_succeeds_before_boot_failure(void)
{
    reset_fakes();
    const esp_err_t script[] = {
        ESP_ERR_TIMEOUT,
        ESP_ERR_INVALID_STATE,
        ESP_OK,
    };
    set_mount_script(script, sizeof(script) / sizeof(script[0]));

    app_storage_boot_run_diagnostic();

    static const test_event_t expected[] = {
        EVENT_SD_MOUNT,
        EVENT_SD_MOUNT,
        EVENT_SD_MOUNT,
        EVENT_SD_PROBE,
        EVENT_SD_DIAGNOSTIC,
    };
    assert_sequence(expected, sizeof(expected) / sizeof(expected[0]));
    assert(s_mount_attempts == 3U);
    assert(s_delay_calls == 2U);
    assert(s_delay_ticks[0] == 150U);
    assert(s_delay_ticks[1] == 150U);
}


static void test_failure_hook_receives_reason_and_cancel_powers_off(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    install_recovery_ui();
    run_diagnostic_until_poweroff();

    assert(s_reason_calls == 1U);
    assert(s_decision_calls == 1U);
    assert(s_seen_reason.reason == APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT);
    assert(strcmp(s_seen_reason.reason_text_code, "FS_MOUNT err=ESP_FAIL fr=13") == 0);
    assert(s_events[3] == EVENT_RECOVERY_REASON);
    assert(s_events[4] == EVENT_RECOVERY_DECISION);
    for (size_t i = 0U; i < s_event_count; ++i) {
        assert(s_events[i] != EVENT_SHOW_ERROR);
    }
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
}


static void test_probe_details_reach_recovery_ui(void)
{
    reset_fakes();
    s_probe_result = ESP_FAIL;
    install_recovery_ui();
    run_diagnostic_until_poweroff();
    assert(s_seen_reason.reason == APP_STORAGE_BOOT_REASON_WRITE_PROBE_FAILED);
    assert(strcmp(s_seen_reason.reason_text_code, "WRITE_PROBE/WRITE err=ESP_FAIL errno=28") == 0);
    assert(s_format_start_calls == 0U);
}

static void test_confirmed_format_success_continues_without_poweroff(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_decision = APP_STORAGE_RECOVERY_DECISION_FORMAT;
    install_recovery_ui();

    app_storage_boot_run_diagnostic();

    assert(s_reason_calls == 1U);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT);
    assert(s_format_start_calls == 1U);
    assert(s_format_status_calls == 1U);
    assert(s_show_result_calls == 1U);
    assert(s_last_result_ok);
    assert(s_storage_init_calls == 0U);
    assert(s_events[s_event_count - 1U] == EVENT_SD_DIAGNOSTIC);
    for (size_t i = 0U; i < s_event_count; ++i) {
        assert(s_events[i] != EVENT_POWER_SHUTDOWN);
        assert(s_events[i] != EVENT_IDLE);
    }
}


static void test_confirmed_format_failure_falls_back_to_poweroff(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_decision_script[0] = APP_STORAGE_RECOVERY_DECISION_FORMAT;
    s_decision_script[1] = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    s_decision_script_count = 2U;
    s_format_status = (sd_card_format_status_t){
        .state = SD_CARD_FORMAT_STATE_FAILED,
        .percent = 42U,
        .err = ESP_FAIL,
        .fresult = 14,
    };
    install_recovery_ui();

    run_diagnostic_until_poweroff();

    assert(s_format_start_calls == 1U);
    assert(s_show_result_calls == 1U);
    assert(!s_last_result_ok);
    assert(s_last_result_err == ESP_FAIL);
    assert(s_reason_calls == 2U);
    assert(s_decision_calls == 2U);
    size_t result_index = 0U;
    size_t retry_prompt_index = 0U;
    for (size_t i = 0U; i < s_event_count; ++i) {
        if (s_events[i] == EVENT_RECOVERY_RESULT) {
            result_index = i;
        }
        if (s_events[i] == EVENT_RECOVERY_REASON && result_index > 0U) {
            retry_prompt_index = i;
            break;
        }
    }
    assert(result_index > 0U);
    assert(retry_prompt_index > result_index);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
}


static void test_layout_format_success_retries_storage_init_once(void)
{
    reset_fakes();
    s_decision = APP_STORAGE_RECOVERY_DECISION_FORMAT;
    install_recovery_ui();

    app_storage_boot_recover_storage_manager_failure(ESP_FAIL);

    assert(s_reason_calls == 1U);
    assert(s_seen_reason.reason == APP_STORAGE_BOOT_REASON_LAYOUT_INIT_FAILED);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT);
    assert(s_format_start_calls == 1U);
    assert(s_storage_init_calls == 1U);
    assert(s_events[s_event_count - 1U] == EVENT_STORAGE_INIT);
    for (size_t i = 0U; i < s_event_count; ++i) {
        assert(s_events[i] != EVENT_POWER_SHUTDOWN);
        assert(s_events[i] != EVENT_IDLE);
    }
}

int main(void)
{
    test_arm_success_waits_before_cleanup();
    test_arm_failure_skips_unreachable_wait_but_keeps_cleanup();
    test_display_failure_still_arms_and_waits();
    test_mount_failure_uses_same_safe_shutdown_helper();
    test_write_failure_uses_same_safe_shutdown_helper();
    test_mount_retry_succeeds_before_boot_failure();
    test_failure_hook_receives_reason_and_cancel_powers_off();
    test_probe_details_reach_recovery_ui();
    test_confirmed_format_success_continues_without_poweroff();
    test_confirmed_format_failure_falls_back_to_poweroff();
    test_layout_format_success_retries_storage_init_once();
    puts("APP_STORAGE_BOOT_FAILURE_HOST_PASS");
    return 0;
}
