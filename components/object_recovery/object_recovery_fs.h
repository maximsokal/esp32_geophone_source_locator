#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


#define OBJECT_RECOVERY_PATH_SIZE          256
#define OBJECT_RECOVERY_METADATA_MAX_SIZE 16384

typedef struct
{
    const char *primary;
    const char *backup;
    const char *temporary;
} object_recovery_metadata_attempts_t;


esp_err_t object_recovery_fs_build_path(
    char *buffer,
    size_t buffer_size,
    const char *directory,
    const char *file_name
);


bool object_recovery_fs_metadata_valid(
    const char *path,
    const char *expected_id,
    const char **reason_out
);


esp_err_t object_recovery_fs_unlink_if_exists(
    const char *path
);


esp_err_t object_recovery_fs_restore_file(
    const char *source,
    const char *destination
);


esp_err_t object_recovery_fs_prepare_metadata(
    const char *object_directory,
    const char *object_id,
    bool *valid,
    bool *recovered,
    object_recovery_metadata_attempts_t *attempts
);
