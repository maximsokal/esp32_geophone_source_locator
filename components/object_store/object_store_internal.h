#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "object_store.h"


/*
 * Object-store state helpers.
 *
 * Filesystem-backed operations use one lock order everywhere:
 * storage operation guard first, object-store mutex second.
 */
esp_err_t object_store_internal_begin_file_operation(void);


void object_store_internal_end_file_operation(void);


const char *object_store_internal_active_id(void);


void object_store_internal_set_active_id(
    const char *object_id
);


void object_store_internal_clear_active_id(void);


void object_store_internal_fill_summary(
    object_store_summary_t *object,
    const char *object_id,
    bool active
);


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


esp_err_t object_store_fs_clear_active(void);


esp_err_t object_store_fs_delete_object(
    const char *object_id
);


esp_err_t object_store_json_write_metadata(
    const char *object_id
);
