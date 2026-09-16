#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "display.h"
#include "esp_err.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "ui.h"


typedef enum {
    EVENT_SD_MOUNT = 1,
    EVENT_SD_DIAGNOSTIC,
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
static esp_err_t s_diagnostic_result;
static esp_err_t s_display_result;
static esp_err_t s_ui_result;
static esp_err_t s_arm_result;
static char s_error_message[64];
static jmp_buf s_idle_jump;
static bool s_jump_from_idle;

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
    s_diagnostic_result = ESP_OK;
    s_display_result = ESP_OK;
    s_ui_result = ESP_OK;
    s_arm_result = ESP_OK;
    s_error_message[0] = '\0';
    s_jump_from_idle = false;
}

esp_err_t sd_card_mount(void)
{
    push_event(EVENT_SD_MOUNT);
    return s_mount_result;
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

esp_err_t display_init(void)
{
    push_event(EVENT_DISPLAY_INIT);
    return s_display_result;
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

    assert(s_events[0] == EVENT_SD_MOUNT);
    assert(s_events[1] == EVENT_DISPLAY_INIT);
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
    assert(s_events[1] == EVENT_SD_DIAGNOSTIC);
    assert(s_events[2] == EVENT_DISPLAY_INIT);
    assert(strcmp(s_error_message, "WRITE TEST FAILED") == 0);
    assert(s_events[s_event_count - 2U] == EVENT_POWER_SHUTDOWN);
    assert(s_events[s_event_count - 1U] == EVENT_IDLE);
}

int main(void)
{
    test_arm_success_waits_before_cleanup();
    test_arm_failure_skips_unreachable_wait_but_keeps_cleanup();
    test_display_failure_still_arms_and_waits();
    test_mount_failure_uses_same_safe_shutdown_helper();
    test_write_failure_uses_same_safe_shutdown_helper();
    puts("APP_STORAGE_BOOT_FAILURE_HOST_PASS");
    return 0;
}
