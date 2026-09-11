#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


#ifndef STORAGE_MANAGER_MOUNT_POINT
#define STORAGE_MANAGER_MOUNT_POINT \
    "/sdcard"
#endif

#ifndef STORAGE_MANAGER_ROOT_PATH
#define STORAGE_MANAGER_ROOT_PATH \
    STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#endif

#ifndef STORAGE_MANAGER_OBJECTS_PATH
#define STORAGE_MANAGER_OBJECTS_PATH \
    STORAGE_MANAGER_ROOT_PATH "/objects"
#endif


esp_err_t storage_manager_init(void);


esp_err_t storage_manager_shutdown(void);


bool storage_manager_is_ready(void);


/*
 * Acquire the recursive high-level storage operation guard.
 *
 * The guard serializes complete filesystem operations across
 * object services, storage services and future acquisition code.
 *
 * Calls may be nested by the same task. Every successful begin
 * must be paired with storage_manager_operation_end().
 */
esp_err_t storage_manager_operation_begin(void);


void storage_manager_operation_end(void);


esp_err_t storage_manager_create_directory(
    const char *path
);


/*
 * Create one directory and fail if the path already exists.
 *
 * ESP_ERR_INVALID_STATE denotes an EEXIST collision. This is intended for
 * resources whose ownership must be established by the current operation.
 */
esp_err_t storage_manager_create_directory_exclusive(
    const char *path
);


bool storage_manager_path_exists(
    const char *path
);


/*
 * Recursively remove one file or directory tree.
 *
 * ESP_ERR_NOT_FOUND is returned when the root path does not exist.
 */
esp_err_t storage_manager_remove_tree(
    const char *path
);


/*
 * Delete all application data under /sdcard/geolocator
 * and recreate the empty application directory layout.
 *
 * The SD filesystem itself is not formatted.
 */
esp_err_t storage_manager_erase_application_data(void);


/*
 * Write a small metadata file using a temporary file
 * and backup replacement.
 */
esp_err_t storage_manager_atomic_write_text(
    const char *path,
    const char *text
);


/*
 * Read a complete small text file into a caller buffer.
 */
esp_err_t storage_manager_read_text(
    const char *path,
    char *buffer,
    size_t buffer_size
);


/*
 * Read a text file whose primary path was already established by the
 * caller's directory census. This preserves the normal read semantics while
 * avoiding a redundant primary/.bak recovery probe for an existing file.
 */
esp_err_t storage_manager_read_text_known_present(
    const char *path,
    char *buffer,
    size_t buffer_size
);
