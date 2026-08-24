#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


#define STORAGE_MANAGER_MOUNT_POINT \
    "/sdcard"

#define STORAGE_MANAGER_ROOT_PATH \
    STORAGE_MANAGER_MOUNT_POINT "/geolocator"

#define STORAGE_MANAGER_OBJECTS_PATH \
    STORAGE_MANAGER_ROOT_PATH "/objects"


esp_err_t storage_manager_init(void);


esp_err_t storage_manager_shutdown(void);


bool storage_manager_is_ready(void);


esp_err_t storage_manager_create_directory(
    const char *path
);


bool storage_manager_path_exists(
    const char *path
);


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