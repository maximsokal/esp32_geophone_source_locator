#pragma once

#include <stddef.h>

#include "esp_err.h"

#define STORAGE_MANAGER_MOUNT_POINT "/sdcard"
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text);
esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size);

/* The listing functions hold the storage operation guard around their file access; the host has no SD card to guard. */
static inline esp_err_t storage_manager_operation_begin(void) { return ESP_OK; }
static inline void storage_manager_operation_end(void) {}
