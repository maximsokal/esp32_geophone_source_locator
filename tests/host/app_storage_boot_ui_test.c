#include "app_storage_boot_ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "freertos/queue.h"
#include "power_control.h"
#include "ui.h"
#include "ui_i18n_nvs.h"


typedef struct {
    keypad_key_t key;
    bool shutdown_notification;
} fake_queue_item_t;


typedef struct fake_queue {
    fake_queue_item_t items[8];
    size_t head;
    size_t count;
} fake_queue_t;


static fake_queue_t s_queue;
static app_storage_recovery_ui_t s_installed_ui;
static bool s_display_ready;
static unsigned s_display_init_calls;
static unsigned s_ui_init_calls;
static unsigned s_locale_load_calls;
static ui_locale_t s_saved_locale = UI_LOCALE_UK;
static keypad_event_callback_t s_key_callback;
static void *s_key_context;
static unsigned s_key_monitor_starts;
static unsigned s_key_monitor_stops;
static esp_err_t s_key_monitor_start_result;
static power_control_shutdown_notification_hook_t s_shutdown_hook;
static void *s_shutdown_hook_context;
static esp_err_t s_shutdown_arm_result;
static unsigned s_shutdown_arm_calls;
static bool s_shutdown_on_blocking_receive;
static bool s_receive_fail_next;
static char s_reason_text[64];
static char s_technical_text[96];
static char s_warning_text[64];
static char s_primary_hint[64];
static char s_retry_hint[64];
static char s_cancel_hint[64];
static unsigned s_progress_values[8];
static size_t s_progress_count;
static bool s_last_result_success;
static bool s_last_result_formatted;
static char s_last_error_name[32];
static bool s_blocking_receive_seen;


QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    assert(length == 5U);
    assert(item_size == sizeof(fake_queue_item_t));
    memset(&s_queue, 0, sizeof(s_queue));
    return &s_queue;
}


BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue == &s_queue);
    queue->head = 0U;
    queue->count = 0U;
    return pdTRUE;
}


BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks)
{
    assert(queue == &s_queue);
    assert(item != NULL);
    assert(wait_ticks == 0U);
    if (queue->count >= sizeof(queue->items) / sizeof(queue->items[0])) {
        return pdFALSE;
    }

    const size_t tail = (queue->head + queue->count) %
        (sizeof(queue->items) / sizeof(queue->items[0]));
    queue->items[tail] = *(const fake_queue_item_t *)item;
    ++queue->count;
    return pdTRUE;
}


BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks)
{
    assert(queue == &s_queue);
    assert(item != NULL);
    s_blocking_receive_seen = wait_ticks == portMAX_DELAY;
    if (s_receive_fail_next) {
        s_receive_fail_next = false;
        return pdFALSE;
    }
    if (queue->count == 0U && s_shutdown_on_blocking_receive) {
        s_shutdown_on_blocking_receive = false;
        assert(s_shutdown_hook != NULL);
        s_shutdown_hook(s_shutdown_hook_context);
    }
    if (queue->count == 0U) {
        return pdFALSE;
    }

    *(fake_queue_item_t *)item = queue->items[queue->head];
    queue->head = (queue->head + 1U) %
        (sizeof(queue->items) / sizeof(queue->items[0]));
    --queue->count;
    return pdTRUE;
}


bool display_is_ready(void)
{
    return s_display_ready;
}


esp_err_t display_init(void)
{
    ++s_display_init_calls;
    s_display_ready = true;
    return ESP_OK;
}


esp_err_t ui_boot_init(void)
{
    ++s_ui_init_calls;
    return ESP_OK;
}


void ui_boot_show_storage_recovery(
    const char *reason,
    const char *technical_code,
    const char *warning,
    const char *primary_hint,
    const char *retry_hint,
    const char *cancel_hint
)
{
    (void)snprintf(s_reason_text, sizeof(s_reason_text), "%s", reason);
    (void)snprintf(s_technical_text, sizeof(s_technical_text), "%s", technical_code);
    (void)snprintf(s_warning_text, sizeof(s_warning_text), "%s", warning != NULL ? warning : "");
    (void)snprintf(s_primary_hint, sizeof(s_primary_hint), "%s", primary_hint);
    (void)snprintf(s_retry_hint, sizeof(s_retry_hint), "%s", retry_hint);
    (void)snprintf(s_cancel_hint, sizeof(s_cancel_hint), "%s", cancel_hint);
}


void ui_boot_show_format_progress(unsigned percent)
{
    assert(s_progress_count < sizeof(s_progress_values) / sizeof(s_progress_values[0]));
    s_progress_values[s_progress_count++] = percent;
}


void ui_boot_show_recovery_result(
    bool success,
    bool formatted,
    const char *error_name
)
{
    s_last_result_success = success;
    s_last_result_formatted = formatted;
    (void)snprintf(s_last_error_name, sizeof(s_last_error_name), "%s", error_name);
}


ui_locale_t ui_i18n_nvs_load_locale(void)
{
    ++s_locale_load_calls;
    return s_saved_locale;
}


esp_err_t keypad_start_boot_event_monitor(
    keypad_event_callback_t callback,
    void *user_context
)
{
    assert(callback != NULL);
    s_key_callback = callback;
    s_key_context = user_context;
    ++s_key_monitor_starts;
    return s_key_monitor_start_result;
}


void keypad_stop_boot_event_monitor(void)
{
    s_key_callback = NULL;
    s_key_context = NULL;
    ++s_key_monitor_stops;
}


void app_storage_boot_set_recovery_ui(const app_storage_recovery_ui_t *ui)
{
    assert(ui != NULL);
    s_installed_ui = *ui;
}


esp_err_t power_control_set_shutdown_notification_hook(
    power_control_shutdown_notification_hook_t hook,
    void *user_context
)
{
    s_shutdown_hook = hook;
    s_shutdown_hook_context = user_context;
    return ESP_OK;
}


esp_err_t power_control_arm_shutdown_button(void)
{
    ++s_shutdown_arm_calls;
    return s_shutdown_arm_result;
}


static void test_key_mapping_requires_release_and_respects_action(void)
{
    app_storage_recovery_decision_t decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
    assert(!app_storage_boot_ui_map_key(
        APP_STORAGE_BOOT_ACTION_OFFER_FORMAT,
        KEYPAD_KEY_OK,
        false,
        &decision
    ));

    assert(app_storage_boot_ui_map_key(
        APP_STORAGE_BOOT_ACTION_OFFER_FORMAT,
        KEYPAD_KEY_OK,
        true,
        &decision
    ));
    assert(decision == APP_STORAGE_RECOVERY_DECISION_FORMAT);
    assert(!app_storage_boot_ui_map_key(
        APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY,
        KEYPAD_KEY_OK,
        true,
        &decision
    ));

    const keypad_key_t cancel_keys[] = {
        KEYPAD_KEY_LEFT,
        KEYPAD_KEY_UP,
        KEYPAD_KEY_DOWN,
    };
    for (size_t i = 0U; i < sizeof(cancel_keys) / sizeof(cancel_keys[0]); ++i) {
        assert(app_storage_boot_ui_map_key(
            APP_STORAGE_BOOT_ACTION_OFFER_FORMAT,
            cancel_keys[i],
            true,
            &decision
        ));
        assert(decision == APP_STORAGE_RECOVERY_DECISION_CANCEL);
    }

    assert(app_storage_boot_ui_map_key(
        APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY,
        KEYPAD_KEY_RIGHT,
        true,
        &decision
    ));
    assert(decision == APP_STORAGE_RECOVERY_DECISION_RETRY);

    assert(app_storage_boot_ui_progress_value(20U, 12U, false) == 20U);
    assert(app_storage_boot_ui_progress_value(98U, 100U, false) == 99U);
    assert(app_storage_boot_ui_progress_value(99U, 12U, true) == 100U);
    assert(app_storage_boot_ui_progress_value(100U, 0U, false) == 100U);
}


static void test_action_copy_does_not_offer_format_for_no_card(void)
{
    app_storage_boot_ui_copy_t copy;
    assert(app_storage_boot_ui_select_copy(
        APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE,
        APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY,
        &copy
    ));
    assert(!copy.show_format_warning);
    assert(copy.warning_key == UI_I18N_KEY_COUNT);
    assert(copy.reason_key == UI_I18N_KEY_BOOT_REASON_CARD_UNAVAILABLE);
    assert(copy.primary_hint_key == UI_I18N_KEY_BOOT_RECOVERY_INSERT_CARD);
    assert(copy.retry_hint_key == UI_I18N_KEY_BOOT_RECOVERY_RETRY_HINT);
    assert(copy.cancel_hint_key == UI_I18N_KEY_BOOT_RECOVERY_CANCEL_HINT);

    assert(app_storage_boot_ui_select_copy(
        APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED,
        APP_STORAGE_BOOT_ACTION_OFFER_FORMAT,
        &copy
    ));
    assert(copy.show_format_warning);
    assert(copy.warning_key == UI_I18N_KEY_BOOT_RECOVERY_WARNING);
    assert(copy.primary_hint_key == UI_I18N_KEY_BOOT_RECOVERY_FORMAT_HINT);
}


static void test_boot_display_and_ui_init_are_idempotent(void)
{
    assert(app_storage_boot_ui_ensure_display_initialized() == ESP_OK);
    assert(app_storage_boot_ui_ensure_initialized() == ESP_OK);
    assert(app_storage_boot_ui_ensure_initialized() == ESP_OK);
    assert(s_display_init_calls == 1U);
    assert(s_ui_init_calls == 1U);
    assert(s_locale_load_calls == 1U);
    assert(ui_i18n_get_locale() == UI_LOCALE_UK);
}


static void test_registered_prompt_uses_blocking_queue_and_progress_is_monotonic(void)
{
    app_storage_boot_ui_register();
    assert(s_installed_ui.show_reason != NULL);
    assert(s_installed_ui.wait_decision != NULL);
    assert(s_installed_ui.show_progress != NULL);
    assert(s_installed_ui.show_result != NULL);

    app_storage_boot_policy_result_t reason = {
        .reason = APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE,
        .action = APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY,
    };
    (void)snprintf(
        reason.reason_text_code,
        sizeof(reason.reason_text_code),
        "%s",
        "CARD_INIT err=ESP_ERR_TIMEOUT"
    );
    s_installed_ui.show_reason(NULL, &reason);
    assert(s_display_init_calls == 1U);
    assert(s_ui_init_calls == 1U);
    assert(strcmp(s_reason_text, ui_tr(UI_I18N_KEY_BOOT_REASON_CARD_UNAVAILABLE)) == 0);
    assert(strcmp(s_technical_text, "CARD_INIT err=ESP_ERR_TIMEOUT") == 0);
    assert(s_warning_text[0] == '\0');
    assert(strcmp(s_primary_hint, ui_tr(UI_I18N_KEY_BOOT_RECOVERY_INSERT_CARD)) == 0);
    assert(strcmp(s_retry_hint, ui_tr(UI_I18N_KEY_BOOT_RECOVERY_RETRY_HINT)) == 0);
    assert(strcmp(s_cancel_hint, ui_tr(UI_I18N_KEY_BOOT_RECOVERY_CANCEL_HINT)) == 0);
    assert(s_key_callback != NULL);
    assert(s_shutdown_hook != NULL);
    s_shutdown_on_blocking_receive = true;
    assert(s_installed_ui.wait_decision(
        NULL,
        APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY
    ) == APP_STORAGE_RECOVERY_DECISION_CANCEL);
    assert(s_blocking_receive_seen);
    assert(s_key_callback == NULL);
    assert(s_shutdown_hook == NULL);

    s_installed_ui.show_reason(NULL, &reason);
    assert(s_key_callback != NULL);
    assert(s_shutdown_hook != NULL);
    s_key_callback(KEYPAD_KEY_RIGHT, s_key_context);
    assert(s_installed_ui.wait_decision(
        NULL,
        APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY
    ) == APP_STORAGE_RECOVERY_DECISION_RETRY);
    assert(s_blocking_receive_seen);
    assert(s_key_callback == NULL);
    assert(s_shutdown_hook == NULL);

    reason.reason = APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED;
    reason.action = APP_STORAGE_BOOT_ACTION_OFFER_FORMAT;
    (void)snprintf(reason.reason_text_code, sizeof(reason.reason_text_code), "%s", "FS_MOUNT err=ESP_FAIL fr=13");
    s_installed_ui.show_reason(NULL, &reason);
    assert(strcmp(s_warning_text, ui_tr(UI_I18N_KEY_BOOT_RECOVERY_WARNING)) == 0);
    assert(strcmp(s_primary_hint, ui_tr(UI_I18N_KEY_BOOT_RECOVERY_FORMAT_HINT)) == 0);
    assert(s_key_callback != NULL);
    assert(s_shutdown_hook != NULL);
    s_key_callback(KEYPAD_KEY_OK, s_key_context);
    assert(s_installed_ui.wait_decision(
        NULL,
        APP_STORAGE_BOOT_ACTION_OFFER_FORMAT
    ) == APP_STORAGE_RECOVERY_DECISION_FORMAT);
    assert(s_key_callback == NULL);
    assert(s_shutdown_hook == NULL);
    assert(s_progress_count == 1U);
    assert(s_progress_values[0] == 0U);

    s_installed_ui.show_progress(NULL, 12U);
    s_installed_ui.show_progress(NULL, 8U);
    s_installed_ui.show_progress(NULL, 62U);
    s_installed_ui.show_progress(NULL, 100U);
    assert(s_progress_count == 4U);
    assert(s_progress_values[1] == 12U);
    assert(s_progress_values[2] == 62U);
    assert(s_progress_values[3] == 99U);

    s_installed_ui.show_result(NULL, true, ESP_OK);
    assert(s_progress_count == 5U);
    assert(s_progress_values[s_progress_count - 1U] == 100U);
    assert(s_last_result_success);
    assert(s_last_result_formatted);
    assert(strcmp(s_last_error_name, "ESP_OK") == 0);
    assert(app_storage_boot_ui_has_recovery_view());

    reason.reason = APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE;
    reason.action = APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY;
    s_installed_ui.show_reason(NULL, &reason);
    assert(s_shutdown_hook != NULL);
    s_receive_fail_next = true;
    assert(s_installed_ui.wait_decision(
        NULL,
        APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY
    ) == APP_STORAGE_RECOVERY_DECISION_CANCEL);
    assert(s_shutdown_hook == NULL);
    assert(s_key_callback == NULL);

    s_key_monitor_start_result = ESP_FAIL;
    s_installed_ui.show_reason(NULL, &reason);
    assert(s_shutdown_hook == NULL);
    assert(s_key_callback == NULL);
    s_key_monitor_start_result = ESP_OK;
    assert(s_shutdown_arm_calls == 4U);
}


int main(void)
{
    test_key_mapping_requires_release_and_respects_action();
    test_action_copy_does_not_offer_format_for_no_card();
    test_boot_display_and_ui_init_are_idempotent();
    test_registered_prompt_uses_blocking_queue_and_progress_is_monotonic();
    return 0;
}
