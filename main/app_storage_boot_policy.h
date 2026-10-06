#pragma once

#include <stdint.h>

#include "esp_err.h"


#define APP_STORAGE_BOOT_FR_NO_FILESYSTEM 13
#define APP_STORAGE_BOOT_MIN_CARD_CAPACITY_BYTES 512ULL


typedef enum {
    APP_STORAGE_BOOT_STAGE_NONE = 0,
    APP_STORAGE_BOOT_STAGE_HOST_INIT,
    APP_STORAGE_BOOT_STAGE_CARD_INIT,
    APP_STORAGE_BOOT_STAGE_FS_MOUNT,
    APP_STORAGE_BOOT_STAGE_WRITE_PROBE,
    APP_STORAGE_BOOT_STAGE_LAYOUT_INIT,
} app_storage_boot_stage_t;

typedef enum {
    APP_STORAGE_BOOT_REASON_NONE = 0,
    APP_STORAGE_BOOT_REASON_HOST_INIT_FAILED,
    APP_STORAGE_BOOT_REASON_CARD_UNAVAILABLE,
    APP_STORAGE_BOOT_REASON_FS_MOUNT_FAILED,
    APP_STORAGE_BOOT_REASON_WRITE_PROBE_FAILED,
    APP_STORAGE_BOOT_REASON_LAYOUT_INIT_FAILED,
} app_storage_boot_reason_t;

typedef enum {
    APP_STORAGE_BOOT_ACTION_NONE = 0,
    APP_STORAGE_BOOT_ACTION_OFFER_FORMAT,
    APP_STORAGE_BOOT_ACTION_SHOW_NO_CARD_RETRY,
    APP_STORAGE_BOOT_ACTION_SHOW_CARD_ERROR_RETRY,
} app_storage_boot_action_t;

typedef struct {
    app_storage_boot_stage_t stage;
    esp_err_t err;
    int fresult;
    esp_err_t probe_result;
    const char *probe_stage_name;
    int probe_errno;
    esp_err_t storage_manager_init_result;
    uint64_t card_capacity_bytes;
} app_storage_boot_policy_input_t;

typedef struct {
    app_storage_boot_reason_t reason;
    app_storage_boot_action_t action;
    char reason_text_code[80];
} app_storage_boot_policy_result_t;


/* Side-effect-free policy mapping; fresult is -1 when FatFs did not report one. */
app_storage_boot_policy_result_t app_storage_boot_policy_classify(
    const app_storage_boot_policy_input_t *input
);
