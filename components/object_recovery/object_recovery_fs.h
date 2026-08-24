#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t object_recovery_fs_prepare_metadata(
    const char *object_directory,
    const char *object_id,
    bool *valid,
    bool *recovered
);


esp_err_t object_recovery_fs_remove_tree(
    const char *path
);