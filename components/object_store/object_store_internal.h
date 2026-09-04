#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "object_store.h"

#define OBJECT_STORE_FS_PATH_SIZE  192

bool object_store_internal_is_initialized(void);
void object_store_internal_set_initialized(bool initialized);
void *object_store_internal_mutex_handle(void);
void object_store_internal_set_mutex_handle(void *mutex);
void object_store_internal_restore_active(void);
esp_err_t object_store_internal_begin_state_operation(void);
void object_store_internal_end_state_operation(void);
esp_err_t object_store_internal_begin_file_operation(void);
void object_store_internal_end_file_operation(void);
const char *object_store_internal_active_id(void);
void object_store_internal_set_active_id(const char *object_id);
void object_store_internal_clear_active_id(void);
void object_store_internal_fill_summary(object_store_summary_t *object, const char *object_id, bool active);

esp_err_t object_store_fs_object_path(const char *object_id, char *path, size_t path_size);
esp_err_t object_store_fs_metadata_path(const char *object_id, char *path, size_t path_size);
void object_store_fs_copy_id(char *destination, size_t destination_size, const char *object_id);
esp_err_t object_store_fs_next_id(char *object_id, size_t object_id_size);
bool object_store_fs_object_exists(const char *object_id);
esp_err_t object_store_fs_create_layout(const char *object_id);
esp_err_t object_store_fs_list(object_store_summary_t *objects, size_t max_objects, size_t *object_count, const char *active_object_id);
esp_err_t object_store_fs_load_active(char *object_id, size_t object_id_size);
esp_err_t object_store_fs_save_active(const char *object_id);
esp_err_t object_store_fs_clear_active(void);
esp_err_t object_store_fs_delete_object(const char *object_id);

esp_err_t object_store_json_write_metadata(const char *object_id);
esp_err_t object_store_json_read_name_locked(const char *object_id, char *name, size_t name_size);
esp_err_t object_store_json_read_profile_locked(const char *object_id, bool active, object_store_profile_t *profile);
esp_err_t object_store_settings_load_locked(float lengths_m[OBJECT_STORE_POD_COUNT]);

/* RAM index over SD metadata. All *_locked hooks require the object-store mutex. */
void object_store_profile_cache_reset(void);
void object_store_profile_cache_note_create_locked(const char *object_id);
void object_store_profile_cache_note_delete_locked(const char *object_id);
void object_store_profile_cache_note_active_locked(const char *object_id);
void object_store_profile_cache_note_rename_locked(const char *object_id, const char *name);
void object_store_profile_cache_note_location_locked(const char *object_id, const object_store_location_t *location);
void object_store_profile_cache_note_cable_locked(const char *object_id, size_t pod_index, float length_m);
void object_store_profile_cache_note_default_locked(size_t pod_index, float length_m);
