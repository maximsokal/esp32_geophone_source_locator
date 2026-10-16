#include "object_recovery_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "object_format.h"
#include "object_recovery.h"
#include "object_recovery_fs.h"
#include "object_store.h"
#include "storage_manager.h"


#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif


static const char *TAG =
    "OBJECT_RECOVERY";

static uint32_t s_boot_id;
static object_recovery_deletion_record_t s_last_deletion_record;

#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"

uint32_t object_recovery_boot_id(void)
{
    if (s_boot_id == 0U) {
        s_boot_id = esp_random();
        if (s_boot_id == 0U) s_boot_id = 1U;
    }
    return s_boot_id;
}

bool object_recovery_get_last_deletion_record(
    object_recovery_deletion_record_t *record_out
)
{
    if (record_out == NULL) return false;
    *record_out = s_last_deletion_record;
    return record_out->valid;
}


static void object_recovery_log_boot_timing(
    const char *object_id,
    const char *stage,
    uint32_t started_ms
)
{
    ESP_LOGI(
        TAG,
        "BOOT_TIMING: object=%s %s=%u ms",
        object_id,
        stage,
        (unsigned)(
            esp_log_timestamp() -
            started_ms
        )
    );
}


esp_err_t object_recovery_background_delete_guard(
    const char *object_id
)
{
    if (!object_format_id_is_valid(object_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    char active_id[OBJECT_FORMAT_ID_SIZE] = {0};
    esp_err_t err = storage_manager_read_text(
        ACTIVE_OBJECT_PATH,
        active_id,
        sizeof(active_id)
    );

    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Cannot determine ACTIVE authority before recovery delete object=%s err=%s",
            object_id,
            esp_err_to_name(err)
        );
        return err;
    }

    active_id[strcspn(active_id, "\r\n\t ")] = '\0';
    if (!object_format_id_is_valid(active_id)) {
        ESP_LOGW(
            TAG,
            "Ignoring invalid persisted ACTIVE marker while guarding recovery delete"
        );
        return ESP_OK;
    }

    if (strcmp(active_id, object_id) == 0) {
        ESP_LOGE(
            TAG,
            "Deferred corrupt-object removal because object is ACTIVE: %s",
            object_id
        );
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


static esp_err_t object_recovery_background_lease_guard(
    const char *object_id
)
{
    bool leased = false;
    const esp_err_t err = object_store_object_is_leased(
        object_id,
        &leased
    );
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Cannot determine object lease before recovery delete object=%s err=%s",
            object_id,
            esp_err_to_name(err)
        );
        return err;
    }

    if (leased) {
        ESP_LOGW(
            TAG,
            "Deferred corrupt-object removal because object is leased: %s",
            object_id
        );
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


static esp_err_t object_recovery_check_one_with_policy(
    const char *object_id,
    bool *removed,
    bool boot_recovery
)
{
    if (
        object_id == NULL ||
        !object_format_id_is_valid(object_id) ||
        removed == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *removed =
        false;

    char *object_directory = malloc(
        OBJECT_RECOVERY_PATH_SIZE
    );
    if (object_directory == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int written =
        snprintf(
            object_directory,
            OBJECT_RECOVERY_PATH_SIZE,
            "%s/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );

    if (
        written < 0 ||
        written >=
            OBJECT_RECOVERY_PATH_SIZE
    ) {
        free(object_directory);
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat info;

    uint32_t stage_started_ms =
        esp_log_timestamp();

    const int stat_result =
        stat(
            object_directory,
            &info
        );

    object_recovery_log_boot_timing(
        object_id,
        "directory_stat",
        stage_started_ms
    );

    if (stat_result != 0) {
        free(object_directory);
        return ESP_FAIL;
    }

    if (
        !S_ISDIR(
            info.st_mode
        )
    ) {
        free(object_directory);
        return ESP_OK;
    }

    bool valid =
        false;

    bool recovered =
        false;
    object_recovery_metadata_attempts_t attempts = {0};

    stage_started_ms =
        esp_log_timestamp();

    esp_err_t err =
        object_recovery_fs_prepare_metadata(
            object_directory,
            object_id,
            &valid,
            &recovered,
            &attempts
        );

    object_recovery_log_boot_timing(
        object_id,
        "prepare_metadata",
        stage_started_ms
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Metadata recovery failed for %s",
            object_id
        );

        free(object_directory);
        return err;
    }

    if (valid) {
        if (recovered) {
            ESP_LOGW(
                TAG,
                "Recovered metadata for %s",
                object_id
            );
        }

        free(object_directory);
        return ESP_OK;
    }

    ESP_LOGW(
        TAG,
        "Incomplete object found: %s",
        object_id
    );

    /*
     * Check persisted ACTIVE authority at every destructive boundary. Runtime
     * background recovery also checks process-lifetime leases while holding
     * the storage operation used by lease acquisition and DELETE; boot has no
     * runtime leases before object_store_init().
     */
    err = object_recovery_background_delete_guard(object_id);
    if (err != ESP_OK) {
        free(object_directory);
        return err;
    }
    if (!boot_recovery) {
        err = object_recovery_background_lease_guard(object_id);
        if (err != ESP_OK) {
            free(object_directory);
            return err;
        }
    }

    object_store_delete_scope_t scope = {0};
    err = object_store_get_delete_scope(object_id, &scope);
    if (err != ESP_OK) {
        free(object_directory);
        return err;
    }

    object_recovery_deletion_record_t record = {0};
    (void)snprintf(record.object_id, sizeof(record.object_id), "%s", object_id);
    (void)snprintf(record.reason, sizeof(record.reason), "%s", "unrecoverable_metadata");
    (void)snprintf(
        record.when,
        sizeof(record.when),
        "%s",
        boot_recovery ? "boot" : "runtime"
    );
    record.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    record.boot_id = object_recovery_boot_id();
    record.sessions = scope.sessions;
    record.artifacts = scope.artifacts;
    record.localization_runs = scope.localization_runs;
    record.bytes = scope.bytes;
    (void)snprintf(
        record.attempts,
        sizeof(record.attempts),
        "primary(object.json)=%s;backup(object.json.bak)=%s;temporary(object.json.tmp)=%s",
        attempts.primary != NULL ? attempts.primary : "unknown",
        attempts.backup != NULL ? attempts.backup : "unknown",
        attempts.temporary != NULL ? attempts.temporary : "unknown"
    );

    stage_started_ms =
        esp_log_timestamp();

    err =
        storage_manager_remove_tree(
            object_directory
        );

    object_recovery_log_boot_timing(
        object_id,
        "remove_incomplete_tree",
        stage_started_ms
    );

    if (err != ESP_OK) {
        free(object_directory);
        return err;
    }

    record.valid = true;
    s_last_deletion_record = record;
    *removed = true;

    ESP_LOGW(
        TAG,
        "OBJECT_RECOVERY_REMOVED reason=%s when=%s uptime_ms=%llu boot_id=%u object=%s sessions=%u artifacts=%u localization_runs=%u bytes=%llu attempts=%s",
        record.reason,
        record.when,
        (unsigned long long)record.uptime_ms,
        (unsigned)record.boot_id,
        record.object_id,
        (unsigned)record.sessions,
        (unsigned)record.artifacts,
        (unsigned)record.localization_runs,
        (unsigned long long)record.bytes,
        record.attempts
    );

    free(object_directory);
    return ESP_OK;
}


esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
)
{
    return object_recovery_check_one_with_policy(
        object_id,
        removed,
        false
    );
}


esp_err_t object_recovery_check_one_background(
    const char *object_id,
    bool *removed
)
{
    return object_recovery_check_one_with_policy(
        object_id,
        removed,
        false
    );
}


esp_err_t object_recovery_check_one_boot(
    const char *object_id,
    bool *removed
)
{
    return object_recovery_check_one_with_policy(
        object_id,
        removed,
        true
    );
}
