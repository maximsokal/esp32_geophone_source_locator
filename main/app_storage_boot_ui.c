#include "app_storage_boot_ui.h"
#include "app_keypad_config.h"

#include <stdatomic.h>

#include "display.h"
#include "power_control.h"
#include "ui.h"
#include "ui_i18n_nvs.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


#define APP_STORAGE_BOOT_UI_KEY_QUEUE_LENGTH 5U


typedef struct {
    keypad_key_t key;
    bool shutdown_notification;
} app_storage_boot_ui_queue_item_t;


static const char *TAG = "APP_STORAGE_BOOT_UI";

#if APP_DISPLAY_ENABLED
static bool s_display_initialized;
#endif
static bool s_boot_ui_initialized;
static bool s_locale_loaded;
static bool s_recovery_view_active;
static bool s_prompt_ready;
static bool s_format_active;
static bool s_format_retry_pending;
static esp_err_t s_format_failure_err;
static unsigned s_progress_percent;
static app_storage_boot_action_t s_prompt_action = APP_STORAGE_BOOT_ACTION_NONE;
static QueueHandle_t s_key_queue;
static atomic_uint_least32_t s_queue_peak_occupancy;
static atomic_uint_least32_t s_queue_occupancy;
static atomic_uint_least32_t s_key_overflow_count;
static atomic_uint_least32_t s_shutdown_overflow_count;


static uint32_t app_storage_boot_ui_saturating_increment(
    atomic_uint_least32_t *counter
)
{
    uint_least32_t current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current < UINT32_MAX && !atomic_compare_exchange_weak_explicit(
        counter,
        &current,
        current + 1U,
        memory_order_relaxed,
        memory_order_relaxed
    )) {
    }
    return (uint32_t)(current < UINT32_MAX ? current + 1U : UINT32_MAX);
}


static void app_storage_boot_ui_record_occupancy(uint32_t occupancy)
{
    if (occupancy > APP_STORAGE_BOOT_UI_KEY_QUEUE_LENGTH) {
        occupancy = APP_STORAGE_BOOT_UI_KEY_QUEUE_LENGTH;
    }
    uint_least32_t current = atomic_load_explicit(
        &s_queue_peak_occupancy, memory_order_relaxed
    );
    while (current < occupancy && !atomic_compare_exchange_weak_explicit(
        &s_queue_peak_occupancy,
        &current,
        occupancy,
        memory_order_relaxed,
        memory_order_relaxed
    )) {
    }
}


static void app_storage_boot_ui_release_occupancy(void)
{
    uint_least32_t current = atomic_load_explicit(
        &s_queue_occupancy, memory_order_relaxed
    );
    while (
        current > 0U && !atomic_compare_exchange_weak_explicit(
            &s_queue_occupancy,
            &current,
            current - 1U,
            memory_order_relaxed,
            memory_order_relaxed
        )
    ) {
    }
}


static bool app_storage_boot_ui_should_log_count(uint32_t count)
{
    return count <= 3U || (count & (count - 1U)) == 0U;
}


static BaseType_t app_storage_boot_ui_send_event(
    QueueHandle_t queue,
    const app_storage_boot_ui_queue_item_t *item
)
{
    const uint32_t occupancy = (uint32_t)atomic_fetch_add_explicit(
        &s_queue_occupancy, 1U, memory_order_relaxed
    ) + 1U;
    app_storage_boot_ui_record_occupancy(occupancy);
    const BaseType_t result = xQueueSend(queue, item, 0U);
    if (result != pdTRUE) {
        app_storage_boot_ui_release_occupancy();
    }
    return result;
}


static void app_storage_boot_ui_show_reason(
    void *context,
    const app_storage_boot_policy_result_t *reason
);

static app_storage_recovery_decision_t app_storage_boot_ui_wait_decision(
    void *context,
    app_storage_boot_action_t action
);

static void app_storage_boot_ui_show_progress(void *context, unsigned percent);

static void app_storage_boot_ui_show_result(
    void *context,
    bool success,
    esp_err_t err
);
static void app_storage_boot_ui_show_continue_without_storage(void *context);

#if APP_KEYPAD_ENABLED
static void app_storage_boot_ui_key_callback(keypad_key_t key, void *context);
#endif
static void app_storage_boot_ui_shutdown_callback(void *context);


static void app_storage_boot_ui_finish_prompt(void)
{
    (void)power_control_set_shutdown_notification_hook(NULL, NULL);
#if APP_KEYPAD_ENABLED
    keypad_stop_boot_event_monitor();
#endif
    if (s_key_queue != NULL) {
        (void)xQueueReset(s_key_queue);
        atomic_store_explicit(&s_queue_occupancy, 0U, memory_order_relaxed);
    }
    s_prompt_ready = false;
    s_prompt_action = APP_STORAGE_BOOT_ACTION_NONE;
}


static const app_storage_recovery_ui_t s_recovery_ui = {
    .context = NULL,
    .show_reason = app_storage_boot_ui_show_reason,
    .wait_decision = app_storage_boot_ui_wait_decision,
    .show_progress = app_storage_boot_ui_show_progress,
    .show_result = app_storage_boot_ui_show_result,
    .show_continue_without_storage = app_storage_boot_ui_show_continue_without_storage,
};


void app_storage_boot_ui_register(void)
{
    s_format_retry_pending = false;
    s_format_failure_err = ESP_OK;
#if APP_KEYPAD_ENABLED
    app_storage_boot_set_recovery_ui(&s_recovery_ui);
#else
    /* Missing-card continuation remains available without interactive recovery input. */
    (void)s_recovery_ui;
    const app_storage_recovery_ui_t recovery_ui = {
        .context = NULL,
        .show_continue_without_storage = app_storage_boot_ui_show_continue_without_storage,
    };
    app_storage_boot_set_recovery_ui(&recovery_ui);
#endif
}


esp_err_t app_storage_boot_ui_ensure_display_initialized(void)
{
#if APP_DISPLAY_ENABLED
    if (!s_display_initialized && display_is_ready()) {
        s_display_initialized = true;
    }

    if (!s_display_initialized) {
        const esp_err_t err = display_init();
        if (err != ESP_OK) {
            return err;
        }
        s_display_initialized = true;
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


esp_err_t app_storage_boot_ui_ensure_initialized(void)
{
    const esp_err_t display_err = app_storage_boot_ui_ensure_display_initialized();
    if (display_err != ESP_OK) {
        return display_err;
    }

    if (!s_locale_loaded) {
        ui_i18n_set_locale(ui_i18n_nvs_load_locale());
        s_locale_loaded = true;
    }

    if (!s_boot_ui_initialized) {
        const esp_err_t err = ui_boot_init();
        if (err != ESP_OK) {
            return err;
        }
        s_boot_ui_initialized = true;
    }

    return ESP_OK;
}


static ui_i18n_key_t app_storage_boot_ui_reason_key(
    app_storage_boot_reason_t reason
)
{
    switch (reason) {
        case APP_STORAGE_BOOT_REASON_HOST_INIT_FAILED:
            return UI_I18N_KEY_BOOT_REASON_HOST_INIT;
        case APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE:
            return UI_I18N_KEY_BOOT_REASON_CARD_UNAVAILABLE;
        case APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED:
            return UI_I18N_KEY_BOOT_REASON_FS_MOUNT;
        case APP_STORAGE_BOOT_REASON_WRITE_PROBE_FAILED:
            return UI_I18N_KEY_BOOT_REASON_WRITE_PROBE;
        case APP_STORAGE_BOOT_REASON_LAYOUT_INIT_FAILED:
            return UI_I18N_KEY_BOOT_REASON_LAYOUT_INIT;
        case APP_STORAGE_BOOT_REASON_NONE:
        default:
            return UI_I18N_KEY_COUNT;
    }
}


bool app_storage_boot_ui_select_copy(
    app_storage_boot_reason_t reason,
    app_storage_boot_action_t action,
    app_storage_boot_ui_copy_t *out_copy
)
{
    if (out_copy == NULL) {
        return false;
    }

    const ui_i18n_key_t reason_key = app_storage_boot_ui_reason_key(reason);
    if (reason_key == UI_I18N_KEY_COUNT) {
        return false;
    }

    app_storage_boot_ui_copy_t copy = {
        .reason_key = reason_key,
        .warning_key = UI_I18N_KEY_COUNT,
        .primary_hint_key = UI_I18N_KEY_COUNT,
        .retry_hint_key = UI_I18N_KEY_BOOT_RECOVERY_RETRY_HINT,
        .cancel_hint_key = UI_I18N_KEY_BOOT_RECOVERY_CANCEL_HINT,
        .show_format_warning = false,
    };

    if (action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT) {
        copy.warning_key = UI_I18N_KEY_BOOT_RECOVERY_WARNING;
        copy.primary_hint_key = UI_I18N_KEY_BOOT_RECOVERY_FORMAT_HINT;
        copy.show_format_warning = true;
    } else if (action == APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY) {
        copy.primary_hint_key = UI_I18N_KEY_BOOT_RECOVERY_INSERT_CARD;
    } else if (action == APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY) {
        copy.reason_key = UI_I18N_KEY_BOOT_REASON_FS_MOUNT;
        copy.primary_hint_key = UI_I18N_KEY_BOOT_RECOVERY_RETRY_HINT;
        copy.retry_hint_key = UI_I18N_KEY_BOOT_ERROR_FOOTER;
    } else {
        return false;
    }

    *out_copy = copy;
    return true;
}


bool app_storage_boot_ui_map_key(
    app_storage_boot_action_t action,
    keypad_key_t key,
    bool initial_release_confirmed,
    app_storage_recovery_decision_t *out_decision
)
{
    if (!initial_release_confirmed || out_decision == NULL) {
        return false;
    }

    switch (key) {
        case KEYPAD_KEY_LEFT:
        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
            *out_decision = APP_STORAGE_RECOVERY_DECISION_CANCEL;
            return true;

        case KEYPAD_KEY_RIGHT:
            *out_decision = APP_STORAGE_RECOVERY_DECISION_RETRY;
            return true;

        case KEYPAD_KEY_OK:
            if (action != APP_STORAGE_BOOT_ACTION_OFFER_FORMAT) {
                return false;
            }
            *out_decision = APP_STORAGE_RECOVERY_DECISION_FORMAT;
            return true;

        case KEYPAD_KEY_NONE:
        default:
            return false;
    }
}


unsigned app_storage_boot_ui_progress_value(
    unsigned previous_percent,
    unsigned reported_percent,
    bool success
)
{
    if (success) {
        return 100U;
    }

    if (previous_percent >= 100U) {
        return 100U;
    }
    if (reported_percent > 99U) {
        reported_percent = 99U;
    }
    return reported_percent > previous_percent
        ? reported_percent
        : previous_percent;
}


bool app_storage_boot_ui_has_recovery_view(void)
{
    return s_recovery_view_active;
}


esp_err_t app_storage_boot_ui_get_queue_stats(
    app_storage_boot_ui_queue_stats_t *stats
)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    stats->peak_occupancy = (uint32_t)atomic_load_explicit(
        &s_queue_peak_occupancy, memory_order_relaxed
    );
    stats->key_overflow_count = (uint32_t)atomic_load_explicit(
        &s_key_overflow_count, memory_order_relaxed
    );
    stats->shutdown_overflow_count = (uint32_t)atomic_load_explicit(
        &s_shutdown_overflow_count, memory_order_relaxed
    );
    return ESP_OK;
}


#if APP_KEYPAD_ENABLED
static void app_storage_boot_ui_key_callback(keypad_key_t key, void *context)
{
    (void)context;
    if (s_key_queue != NULL) {
        app_storage_boot_ui_queue_item_t item;
        item.key = key;
        item.shutdown_notification = false;
        if (app_storage_boot_ui_send_event(s_key_queue, &item) != pdTRUE) {
            const uint32_t count = app_storage_boot_ui_saturating_increment(
                &s_key_overflow_count
            );
            if (app_storage_boot_ui_should_log_count(count)) {
                ESP_LOGW(TAG, "key event dropped queue_full count=%u", (unsigned)count);
            }
        }
    }
}
#endif


static void app_storage_boot_ui_shutdown_callback(void *context)
{
    QueueHandle_t queue = (QueueHandle_t)context;
    if (queue != NULL) {
        app_storage_boot_ui_queue_item_t item;
        item.key = KEYPAD_KEY_NONE;
        item.shutdown_notification = true;
        if (app_storage_boot_ui_send_event(queue, &item) != pdTRUE) {
            const uint32_t count = app_storage_boot_ui_saturating_increment(
                &s_shutdown_overflow_count
            );
            app_storage_boot_ui_queue_item_t displaced;
            /* Callbacks must stay nonblocking; free one slot for shutdown priority. */
            const bool freed_slot = xQueueReceive(queue, &displaced, 0U) == pdTRUE;
            if (freed_slot) {
                if (!displaced.shutdown_notification) {
                    const uint32_t key_count =
                        app_storage_boot_ui_saturating_increment(&s_key_overflow_count);
                    if (app_storage_boot_ui_should_log_count(key_count)) {
                        ESP_LOGW(TAG, "key event displaced by shutdown count=%u", (unsigned)key_count);
                    }
                }
            }
            if (
                freed_slot &&
                app_storage_boot_ui_send_event(queue, &item) == pdTRUE
            ) {
                if (app_storage_boot_ui_should_log_count(count)) {
                    ESP_LOGW(TAG, "shutdown admitted with priority count=%u", (unsigned)count);
                }
            } else if (app_storage_boot_ui_should_log_count(count)) {
                ESP_LOGE(TAG, "shutdown event lost queue_full count=%u", (unsigned)count);
            }
        }
    }
}


static void app_storage_boot_ui_show_reason(
    void *context,
    const app_storage_boot_policy_result_t *reason
)
{
    (void)context;
    s_prompt_ready = false;
    s_prompt_action = APP_STORAGE_BOOT_ACTION_NONE;
    s_format_active = false;
    s_progress_percent = 0U;
    app_storage_boot_ui_finish_prompt();

    if (reason == NULL || app_storage_boot_ui_ensure_initialized() != ESP_OK) {
        app_storage_boot_ui_finish_prompt();
        return;
    }

    app_storage_boot_ui_copy_t copy;
    if (!app_storage_boot_ui_select_copy(reason->reason, reason->action, &copy)) {
        app_storage_boot_ui_finish_prompt();
        return;
    }

    if (s_key_queue == NULL) {
        s_key_queue = xQueueCreate(
            APP_STORAGE_BOOT_UI_KEY_QUEUE_LENGTH,
            sizeof(app_storage_boot_ui_queue_item_t)
        );
    }
    if (s_key_queue == NULL) {
        app_storage_boot_ui_finish_prompt();
        return;
    }

    (void)xQueueReset(s_key_queue);
    atomic_store_explicit(&s_queue_occupancy, 0U, memory_order_relaxed);
    const bool retrying_failed_format =
        s_format_retry_pending &&
        reason->action == APP_STORAGE_BOOT_ACTION_OFFER_FORMAT;
    ui_boot_show_storage_recovery(
        ui_tr(copy.reason_key),
        retrying_failed_format
            ? esp_err_to_name(s_format_failure_err)
            : reason->reason_text_code,
        retrying_failed_format
            ? ui_tr(UI_I18N_KEY_BOOT_FORMAT_FAILED)
            : (copy.show_format_warning ? ui_tr(copy.warning_key) : NULL),
        retrying_failed_format
            ? ui_tr(UI_I18N_KEY_BOOT_FORMAT_RETRY_HINT)
            : ui_tr(copy.primary_hint_key),
        retrying_failed_format
            ? ui_tr(UI_I18N_KEY_BOOT_FORMAT_POWER_HINT)
            : (copy.retry_hint_key == UI_I18N_KEY_COUNT
                ? ""
                : ui_tr(copy.retry_hint_key)),
        copy.cancel_hint_key == UI_I18N_KEY_COUNT
            ? ""
            : ui_tr(copy.cancel_hint_key)
    );
    s_recovery_view_active = true;

#if APP_KEYPAD_ENABLED
    if (keypad_start_boot_event_monitor(
            app_storage_boot_ui_key_callback,
            NULL
        ) != ESP_OK) {
        app_storage_boot_ui_finish_prompt();
        return;
    }
#else
    app_storage_boot_ui_finish_prompt();
    return;
#endif

    s_prompt_action = reason->action;
    s_prompt_ready = true;

    const esp_err_t hook_err = power_control_set_shutdown_notification_hook(
        app_storage_boot_ui_shutdown_callback,
        s_key_queue
    );
    if (hook_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to install boot recovery shutdown notification: %s",
            esp_err_to_name(hook_err)
        );
    }

    const esp_err_t arm_err = power_control_arm_shutdown_button();
    if (arm_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to arm shutdown button for boot recovery: %s",
            esp_err_to_name(arm_err)
        );
    }
}


static app_storage_recovery_decision_t app_storage_boot_ui_wait_decision(
    void *context,
    app_storage_boot_action_t action
)
{
    (void)context;
    if (
        !s_prompt_ready ||
        s_key_queue == NULL ||
        action != s_prompt_action
    ) {
        app_storage_boot_ui_finish_prompt();
        return APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
    }

    for (;;) {
        app_storage_boot_ui_queue_item_t item;
        item.key = KEYPAD_KEY_NONE;
        item.shutdown_notification = false;
        if (xQueueReceive(s_key_queue, &item, portMAX_DELAY) != pdTRUE) {
            app_storage_boot_ui_finish_prompt();
            return APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
        }
        app_storage_boot_ui_release_occupancy();

        if (item.shutdown_notification) {
            app_storage_boot_ui_finish_prompt();
            return APP_STORAGE_RECOVERY_DECISION_SHUTDOWN;
        }

        app_storage_recovery_decision_t decision;
        if (s_format_retry_pending && item.key == KEYPAD_KEY_RIGHT) {
            continue;
        }
        /* The adapter emits press events only after its power-on release gate. */
        if (!app_storage_boot_ui_map_key(action, item.key, true, &decision)) {
            continue;
        }

        app_storage_boot_ui_finish_prompt();
        if (decision == APP_STORAGE_RECOVERY_DECISION_FORMAT) {
            /* Do not interrupt formatting: main waits on a shutdown latch only after startup returns. */
            s_format_active = true;
            s_format_retry_pending = false;
            s_progress_percent = 0U;
            ui_boot_show_format_progress(0U);
        }
        return decision;
    }
}


static void app_storage_boot_ui_show_progress(void *context, unsigned percent)
{
    (void)context;
    if (!s_format_active) {
        return;
    }

    const unsigned next_percent = app_storage_boot_ui_progress_value(
        s_progress_percent,
        percent,
        false
    );
    if (next_percent != s_progress_percent) {
        s_progress_percent = next_percent;
        ui_boot_show_format_progress(s_progress_percent);
    }
}


static void app_storage_boot_ui_show_result(
    void *context,
    bool success,
    esp_err_t err
)
{
    (void)context;
    if (s_format_active && success) {
        const unsigned next_percent = app_storage_boot_ui_progress_value(
            s_progress_percent,
            100U,
            true
        );
        if (next_percent != s_progress_percent) {
            s_progress_percent = next_percent;
            ui_boot_show_format_progress(s_progress_percent);
        }
    }

    ui_boot_show_recovery_result(
        success,
        s_format_active,
        esp_err_to_name(err)
    );
    if (s_format_active) {
        s_format_retry_pending = !success;
        s_format_failure_err = err;
    }
    s_recovery_view_active = true;
    s_format_active = false;
}


static void app_storage_boot_ui_show_continue_without_storage(void *context)
{
    (void)context;
    if (app_storage_boot_ui_ensure_initialized() != ESP_OK) {
        return;
    }

    ui_boot_show_storage_recovery(
        ui_tr(UI_I18N_KEY_BOOT_CONTINUE_NO_STORAGE),
        "",
        NULL,
        "",
        "",
        ""
    );
    s_recovery_view_active = true;
}
