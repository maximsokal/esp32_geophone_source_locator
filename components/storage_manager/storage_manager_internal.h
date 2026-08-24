#pragma once

#include <stddef.h>

#include "esp_err.h"


#define STORAGE_MANAGER_PATH_BUFFER_SIZE  256


/*
 * Internal helpers are called only while the recursive storage
 * operation guard is already owned by the current task.
 */
esp_err_t storage_manager_ensure_directory_locked(
    const char *path
);


esp_err_t storage_manager_sidecar_path(
    const char *path,
    const char *suffix,
    char *result,
    size_t result_size
);


esp_err_t storage_manager_recover_backup_locked(
    const char *path
);
