#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "object_store.h"


bool object_store_fs_id_is_valid(
    const char *object_id
);


bool object_store_fs_object_exists(
    const char *object_id
);


esp_err_t object_store_fs_next_id(
    char *object_id,
    size_t object_id_size
);


esp_err_t object_store_fs_create_layout(
    const char *object_id
);


esp_err_t object_store_fs_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count,
    const char *active_object_id
);


esp_err_t object_store_fs_load_active(
    char *object_id,
    size_t object_id_size
);


esp_err_t object_store_fs_save_active(
    const char *object_id
);


esp_err_t object_store_json_write_metadata(
    const char *object_id
);