#pragma once

#include <stdbool.h>

#include "app_storage_boot_policy.h"
#include "esp_err.h"


typedef enum {
    APP_STORAGE_RECOVERY_DECISION_CANCEL = 0,
    APP_STORAGE_RECOVERY_DECISION_RETRY,
    APP_STORAGE_RECOVERY_DECISION_FORMAT,
    APP_STORAGE_RECOVERY_DECISION_SHUTDOWN,
} app_storage_recovery_decision_t;

/* UI callbacks receive the classified cause and pollable format progress. */
typedef struct {
    void *context;
    void (*show_reason)(void *context, const app_storage_boot_policy_result_t *reason);
    app_storage_recovery_decision_t (*wait_decision)(
        void *context,
        app_storage_boot_action_t action
    );
    void (*show_progress)(void *context, unsigned percent);
    void (*show_result)(void *context, bool success, esp_err_t err);
    void (*show_continue_without_storage)(void *context);
} app_storage_recovery_ui_t;


/* Pass NULL to restore the built-in power-off behavior. */
void app_storage_boot_set_recovery_ui(const app_storage_recovery_ui_t *ui);

void app_storage_boot_run_diagnostic(void);

/* Called by app_startup after the first storage_manager_init() failure. */
void app_storage_boot_recover_storage_manager_failure(esp_err_t err);
