#define APP_STORAGE_BOOT_TEST_LOG_CAPTURE 1
void app_storage_boot_test_log_capture(const char *tag, const char *format, ...);

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_storage_boot.h"
#include "display.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
/* The inventory FreeRTOS shim supplies the lock API; boot supplies its tick type here. */
typedef unsigned long TickType_t;
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#include "freertos/task.h"
#include "hw_inventory.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "sd_card_format.h"
#include "sd_card_info.h"
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
    EVENT_CONTINUE_WITHOUT_STORAGE,
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
static unsigned s_call_order;
static unsigned s_first_delay_order;
static unsigned s_first_mount_order;
static esp_err_t s_mount_result;
static bool s_mount_stage_override_enabled;
static sd_card_mount_stage_t s_mount_stage_override;
static int s_mount_fresult;
static uint64_t s_card_capacity_bytes;
static esp_err_t s_mount_script[8];
static size_t s_mount_script_count;
static size_t s_mount_attempts;
static unsigned s_delay_calls;
static TickType_t s_delay_ticks[8];
static esp_err_t s_diagnostic_result;
static esp_err_t s_probe_result;
static sd_card_probe_stage_t s_probe_stage;
static int s_probe_errno;
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
static unsigned s_continue_without_storage_calls;
static char s_warning_log[160];

static void push_event(test_event_t event)
{
    assert(s_event_count < sizeof(s_events) / sizeof(s_events[0]));
    s_events[s_event_count++] = event;
}

static void reset_fakes(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0U;
    s_call_order = 0U;
    s_first_delay_order = 0U;
    s_first_mount_order = 0U;
    s_mount_result = ESP_OK;
    s_mount_stage_override_enabled = false;
    s_mount_stage_override = SD_CARD_MOUNT_STAGE_NONE;
    s_mount_fresult = 13;
    s_card_capacity_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    memset(s_mount_script, 0, sizeof(s_mount_script));
    s_mount_script_count = 0U;
    s_mount_attempts = 0U;
    s_delay_calls = 0U;
    memset(s_delay_ticks, 0, sizeof(s_delay_ticks));
    s_diagnostic_result = ESP_OK;
    s_probe_result = ESP_OK;
    s_probe_stage = SD_CARD_PROBE_STAGE_COMPLETE;
    s_probe_errno = 0;
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
    s_continue_without_storage_calls = 0U;
    s_warning_log[0] = '\0';
    app_storage_boot_set_recovery_ui(NULL);
    hw_inventory_init();
}

static void set_mount_script(const esp_err_t *results, size_t count)
{
    assert(count <= sizeof(s_mount_script) / sizeof(s_mount_script[0]));
    memcpy(s_mount_script, results, count * sizeof(s_mount_script[0]));
    s_mount_script_count = count;
}

esp_err_t sd_card_mount_recovery(sd_card_mount_report_t *report)
{
    const unsigned call_order = ++s_call_order;
    if (s_mount_attempts == 0U) {
        s_first_mount_order = call_order;
    }
    push_event(EVENT_SD_MOUNT);
    const esp_err_t result =
        s_mount_attempts < s_mount_script_count
            ? s_mount_script[s_mount_attempts]
            : s_mount_result;
    ++s_mount_attempts;
    if (report != NULL) {
        report->stage = result == ESP_OK
            ? SD_CARD_MOUNT_STAGE_NONE
            : (s_mount_stage_override_enabled
                ? s_mount_stage_override
                : (result == ESP_ERR_TIMEOUT || result == ESP_ERR_INVALID_RESPONSE
                    ? SD_CARD_MOUNT_STAGE_CARD_INIT
                    : SD_CARD_MOUNT_STAGE_FS_MOUNT));
        report->err = result;
        report->fresult = report->stage == SD_CARD_MOUNT_STAGE_FS_MOUNT
            ? s_mount_fresult
            : -1;
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

esp_err_t sd_card_get_info(sd_card_info_t *info)
{
    assert(info != NULL);
    memset(info, 0, sizeof(*info));
    info->capacity_bytes = s_card_capacity_bytes;
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    const unsigned call_order = ++s_call_order;
    if (s_delay_calls == 0U) {
        s_first_delay_order = call_order;
    }
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
            : s_probe_stage,
        .err = s_probe_result,
        .sys_errno = s_probe_result == ESP_OK ? 0 : s_probe_errno,
    };
}

const char *sd_card_probe_stage_name(sd_card_probe_stage_t stage)
{
    if (stage == SD_CARD_PROBE_STAGE_COMPLETE) {
        return "COMPLETE";
    }
    if (stage == SD_CARD_PROBE_STAGE_READ) {
        return "READ";
    }
    return "WRITE";
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

static void fake_show_continue_without_storage(void *context)
{
    (void)context;
    push_event(EVENT_CONTINUE_WITHOUT_STORAGE);
    ++s_continue_without_storage_calls;
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
        .show_continue_without_storage = fake_show_continue_without_storage,
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

void app_storage_boot_test_log_capture(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(s_warning_log, sizeof(s_warning_log), format, args);
    va_end(args);
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


static hw_module_info_t get_sd_inventory(void)
{
    hw_module_info_t info;
    assert(hw_inventory_get(HW_MODULE_SD_CARD, &info));
    return info;
}


static unsigned count_events(test_event_t event)
{
    unsigned count = 0U;
    for (size_t i = 0U; i < s_event_count; ++i) {
        count += s_events[i] == event ? 1U : 0U;
    }
    return count;
}


static void assert_absent_run_continued_without_prompt(void)
{
    assert(s_reason_calls == 0U);
    assert(s_decision_calls == 0U);
    assert(s_continue_without_storage_calls == 1U);
    assert(s_format_start_calls == 0U);
    assert(strcmp(
        s_warning_log,
        "BOOT_NO_STORAGE SD absent, continuing without storage"
    ) == 0);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_ABSENT);
    assert(strcmp(inventory.reason, "no response") == 0);
    assert(count_events(EVENT_SHOW_ERROR) == 0U);
    assert(count_events(EVENT_POWER_SHUTDOWN) == 0U);
    assert(count_events(EVENT_IDLE) == 0U);
    assert(count_events(EVENT_CONTINUE_WITHOUT_STORAGE) == 1U);
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
    s_mount_fresult = 1; /* FatFs FR_DISK_ERR. */
    run_diagnostic_until_poweroff();

    assert(s_mount_attempts == 3U);
    const size_t powerup_delay_count =
        APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U;
    assert(s_delay_calls == 2U + powerup_delay_count);
    if (powerup_delay_count > 0U) {
        assert(s_delay_ticks[0] == pdMS_TO_TICKS(APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS));
    }
    assert(s_delay_ticks[powerup_delay_count] == 150U);
    assert(s_delay_ticks[powerup_delay_count + 1U] == 150U);
    assert(s_events[0] == EVENT_SD_MOUNT);
    assert(s_events[1] == EVENT_SD_MOUNT);
    assert(s_events[2] == EVENT_SD_MOUNT);
    assert(s_events[3] == EVENT_DISPLAY_INIT);
    assert(strcmp(s_error_message, "MOUNT FAILED") == 0);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_FAULT);
    assert(strcmp(inventory.reason, "mount failed") == 0);
}


static void test_other_transient_fatfs_results_retry(void)
{
    const int fresults[] = {3, 15}; /* FR_NOT_READY and FR_TIMEOUT. */

    for (size_t i = 0U; i < sizeof(fresults) / sizeof(fresults[0]); ++i) {
        reset_fakes();
        s_mount_result = ESP_FAIL;
        s_mount_fresult = fresults[i];
        run_diagnostic_until_poweroff();

        assert(s_mount_attempts == 3U);
        const size_t powerup_delay_count =
            APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U;
        assert(s_delay_calls == 2U + powerup_delay_count);
    }
}


static void test_absent_mount_failures_continue_without_prompt(void)
{
    const struct {
        sd_card_mount_stage_t stage;
        esp_err_t err;
    } cases[] = {
        {SD_CARD_MOUNT_STAGE_HOST_INIT, ESP_FAIL},
        {SD_CARD_MOUNT_STAGE_CARD_INIT, ESP_FAIL},
        {SD_CARD_MOUNT_STAGE_CARD_INIT, ESP_ERR_TIMEOUT},
        {SD_CARD_MOUNT_STAGE_CARD_INIT, ESP_ERR_INVALID_RESPONSE},
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset_fakes();
        s_mount_result = cases[i].err;
        s_mount_stage_override_enabled = true;
        s_mount_stage_override = cases[i].stage;
        install_recovery_ui();

        app_storage_boot_run_diagnostic();

        assert(s_mount_attempts == 3U);
        const size_t powerup_delay_count =
            APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U;
        assert(s_delay_calls == 2U + powerup_delay_count);
        assert_absent_run_continued_without_prompt();
    }
}


static void test_early_probe_no_response_continues_without_prompt(void)
{
    const esp_err_t errors[] = {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE};
    for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset_fakes();
        s_probe_result = errors[i];
        s_probe_stage = SD_CARD_PROBE_STAGE_READ;
        install_recovery_ui();

        app_storage_boot_run_diagnostic();

        assert(s_mount_attempts == 1U);
        assert(count_events(EVENT_SD_PROBE) == 1U);
        assert(count_events(EVENT_SD_DIAGNOSTIC) == 0U);
        assert_absent_run_continued_without_prompt();
    }
}


static void test_final_write_read_no_response_continues_without_prompt(void)
{
    const esp_err_t errors[] = {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE};
    for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset_fakes();
        s_diagnostic_result = errors[i];
        install_recovery_ui();

        app_storage_boot_run_diagnostic();

        assert(count_events(EVENT_SD_PROBE) == 1U);
        assert(count_events(EVENT_SD_DIAGNOSTIC) == 1U);
        assert_absent_run_continued_without_prompt();
    }
}


static void test_generic_filesystem_error_is_non_destructive_fault(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_mount_fresult = 1; /* FatFs FR_DISK_ERR. */
    s_decision = APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
    install_recovery_ui();
    run_diagnostic_until_poweroff();

    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_format_start_calls == 0U);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_FAULT);
    assert(strcmp(inventory.reason, "mount failed") == 0);
}


static void test_missing_filesystem_on_tiny_card_does_not_offer_format(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_card_capacity_bytes = APP_STORAGE_BOOT_MIN_CARD_CAPACITY_BYTES - 1U;
    s_decision = APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
    install_recovery_ui();
    run_diagnostic_until_poweroff();

    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_format_start_calls == 0U);
}


static void test_successful_probe_reports_present(void)
{
    reset_fakes();
    app_storage_boot_run_diagnostic();

    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_PRESENT);
    assert(strcmp(inventory.reason, "ready") == 0);
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
    assert(s_delay_calls == (APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U));
}

static void test_mount_retry_succeeds_before_boot_failure(void)
{
    reset_fakes();
    s_mount_fresult = 1; /* The second attempt fails with FR_DISK_ERR. */
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
    const size_t powerup_delay_count =
        APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U;
    assert(s_delay_calls == 2U + powerup_delay_count);
    if (powerup_delay_count > 0U) {
        assert(s_delay_ticks[0] == pdMS_TO_TICKS(APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS));
    }
    assert(s_delay_ticks[powerup_delay_count] == 150U);
    assert(s_delay_ticks[powerup_delay_count + 1U] == 150U);
    assert((powerup_delay_count > 0U) == (s_first_delay_order < s_first_mount_order));
}


static void test_no_filesystem_does_not_retry(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    install_recovery_ui();
    app_storage_boot_run_diagnostic();

    assert(s_mount_attempts == 1U);
    assert(s_delay_calls == (APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U));
    assert(s_reason_calls == 1U);
    assert(s_decision_calls == 1U);
    assert(s_seen_reason.reason == APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT);
    assert(strcmp(s_seen_reason.reason_text_code, "FS_MOUNT err=ESP_FAIL fr=13") == 0);
    assert(s_events[0] == EVENT_SD_MOUNT);
    assert(s_events[1] == EVENT_RECOVERY_REASON);
    assert(s_events[2] == EVENT_RECOVERY_DECISION);
    assert(s_events[3] == EVENT_CONTINUE_WITHOUT_STORAGE);
    assert(s_continue_without_storage_calls == 1U);
    assert(s_format_start_calls == 0U);
    assert(strcmp(
        s_warning_log,
        "BOOT_NO_STORAGE continuing without SD (user cancel)"
    ) == 0);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_FAULT);
    assert(strcmp(inventory.reason, "no filesystem") == 0);
    for (size_t i = 0U; i < s_event_count; ++i) {
        assert(s_events[i] != EVENT_SHOW_ERROR);
        assert(s_events[i] != EVENT_POWER_SHUTDOWN);
        assert(s_events[i] != EVENT_IDLE);
    }
}


static void test_card_error_cancel_keeps_sd_faulted(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_mount_fresult = 1; /* FatFs FR_DISK_ERR. */
    s_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    install_recovery_ui();

    app_storage_boot_run_diagnostic();

    assert(s_reason_calls == 1U);
    assert(s_decision_calls == 1U);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_format_start_calls == 0U);
    assert(s_continue_without_storage_calls == 1U);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_FAULT);
    assert(strcmp(inventory.reason, "mount failed") == 0);
    for (size_t i = 0U; i < s_event_count; ++i) {
        assert(s_events[i] != EVENT_POWER_SHUTDOWN);
        assert(s_events[i] != EVENT_IDLE);
    }
}


static void test_other_deterministic_fatfs_results_do_not_retry(void)
{
    const int fresults[] = {
        2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16, 17, 18, 19,
    };

    for (size_t i = 0U; i < sizeof(fresults) / sizeof(fresults[0]); ++i) {
        reset_fakes();
        s_mount_result = ESP_FAIL;
        s_mount_fresult = fresults[i];
        s_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
        install_recovery_ui();

        app_storage_boot_run_diagnostic();

        assert(s_mount_attempts == 1U);
        assert(s_delay_calls == (APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U));
    }
}


static void test_card_error_retry_still_retries_mount(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_mount_fresult = 1; /* FatFs FR_DISK_ERR. */
    const esp_err_t results[] = {ESP_FAIL, ESP_FAIL, ESP_FAIL, ESP_OK};
    set_mount_script(results, sizeof(results) / sizeof(results[0]));
    s_decision_script[0] = APP_STORAGE_RECOVERY_DECISION_RETRY;
    s_decision_script_count = 1U;
    install_recovery_ui();

    app_storage_boot_run_diagnostic();

    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_reason_calls == 1U);
    assert(s_decision_calls == 1U);
    assert(s_mount_attempts == 4U);
    const size_t powerup_delay_count =
        APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS > 0U ? 1U : 0U;
    assert(s_delay_calls == 2U + powerup_delay_count);
    if (powerup_delay_count > 0U) {
        assert(s_delay_ticks[0] == pdMS_TO_TICKS(APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS));
    }
    assert(s_delay_ticks[powerup_delay_count] == 150U);
    assert(s_delay_ticks[powerup_delay_count + 1U] == 150U);
    assert(s_format_start_calls == 0U);
    assert(s_continue_without_storage_calls == 0U);
    assert(count_events(EVENT_POWER_SHUTDOWN) == 0U);
    assert(count_events(EVENT_IDLE) == 0U);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_PRESENT);
    assert(strcmp(inventory.reason, "ready") == 0);
}


static void test_power_notification_during_recovery_still_powers_off(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;
    s_mount_fresult = 1; /* FatFs FR_DISK_ERR. */
    s_decision = APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
    install_recovery_ui();
    run_diagnostic_until_poweroff();

    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_continue_without_storage_calls == 0U);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
}


static void test_probe_details_reach_recovery_ui(void)
{
    reset_fakes();
    s_probe_result = ESP_FAIL;
    s_probe_stage = SD_CARD_PROBE_STAGE_READ;
    s_probe_errno = ENOENT;
    s_decision = APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
    install_recovery_ui();
    run_diagnostic_until_poweroff();
    assert(s_seen_reason.reason == APP_STORAGE_BOOT_REASON_WRITE_PROBE_FAILED);
    assert(strcmp(s_seen_reason.reason_text_code, "WRITE_PROBE/READ err=ESP_FAIL errno=2") == 0);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_format_start_calls == 0U);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_FAULT);
    assert(strcmp(inventory.reason, "read probe failed") == 0);
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
    /* Keep fatal shutdown coverage on the power-notification path. */
    s_decision_script[1] = APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
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


static void test_layout_error_never_offers_format(void)
{
    reset_fakes();
    hw_inventory_report(HW_MODULE_SD_CARD, HW_STATE_PRESENT, "ready");
    s_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    install_recovery_ui();

    s_jump_from_idle = true;
    if (setjmp(s_idle_jump) == 0) {
        app_storage_boot_recover_storage_manager_failure(ESP_FAIL);
        assert(false);
    }
    s_jump_from_idle = false;

    assert(s_reason_calls == 1U);
    assert(s_seen_reason.reason == APP_STORAGE_BOOT_REASON_LAYOUT_INIT_FAILED);
    assert(s_seen_action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY);
    assert(s_format_start_calls == 0U);
    assert(s_storage_init_calls == 0U);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
    const hw_module_info_t inventory = get_sd_inventory();
    assert(inventory.state == HW_STATE_PRESENT);
}

int main(void)
{
    test_arm_success_waits_before_cleanup();
    test_arm_failure_skips_unreachable_wait_but_keeps_cleanup();
    test_display_failure_still_arms_and_waits();
    test_mount_failure_uses_same_safe_shutdown_helper();
    test_other_transient_fatfs_results_retry();
    test_absent_mount_failures_continue_without_prompt();
    test_early_probe_no_response_continues_without_prompt();
    test_final_write_read_no_response_continues_without_prompt();
    test_generic_filesystem_error_is_non_destructive_fault();
    test_missing_filesystem_on_tiny_card_does_not_offer_format();
    test_successful_probe_reports_present();
    test_write_failure_uses_same_safe_shutdown_helper();
    test_mount_retry_succeeds_before_boot_failure();
    test_no_filesystem_does_not_retry();
    test_card_error_cancel_keeps_sd_faulted();
    test_other_deterministic_fatfs_results_do_not_retry();
    test_card_error_retry_still_retries_mount();
    test_power_notification_during_recovery_still_powers_off();
    test_probe_details_reach_recovery_ui();
    test_confirmed_format_success_continues_without_poweroff();
    test_confirmed_format_failure_falls_back_to_poweroff();
    test_layout_error_never_offers_format();
    puts("APP_STORAGE_BOOT_FAILURE_HOST_PASS");
    return 0;
}
