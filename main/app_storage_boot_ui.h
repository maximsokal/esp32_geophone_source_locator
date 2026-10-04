#pragma once

#include <stdbool.h>

#include "app_storage_boot.h"
#include "esp_err.h"
#include "keypad.h"
#include "ui_i18n.h"


typedef struct {
    ui_i18n_key_t reason_key;
    ui_i18n_key_t warning_key;
    ui_i18n_key_t primary_hint_key;
    ui_i18n_key_t retry_hint_key;
    ui_i18n_key_t cancel_hint_key;
    bool show_format_warning;
} app_storage_boot_ui_copy_t;


/* Install the recovery callbacks before the first SD boot diagnostic. */
void app_storage_boot_ui_register(void);

/* Idempotently initialize the shared display and boot UI. */
esp_err_t app_storage_boot_ui_ensure_display_initialized(void);
esp_err_t app_storage_boot_ui_ensure_initialized(void);

/* Pure selectors used by the callback path and host tests. */
bool app_storage_boot_ui_select_copy(
    app_storage_boot_reason_t reason,
    app_storage_boot_action_t action,
    app_storage_boot_ui_copy_t *out_copy
);

bool app_storage_boot_ui_map_key(
    app_storage_boot_action_t action,
    keypad_key_t key,
    bool initial_release_confirmed,
    app_storage_recovery_decision_t *out_decision
);

unsigned app_storage_boot_ui_progress_value(
    unsigned previous_percent,
    unsigned reported_percent,
    bool success
);

/* True while a recovery prompt/result is the visible boot screen. */
bool app_storage_boot_ui_has_recovery_view(void);
