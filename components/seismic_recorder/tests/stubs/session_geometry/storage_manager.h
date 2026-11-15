#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define STORAGE_MANAGER_MOUNT_POINT "seismic_recorder_session_geometry_tmp"
#define STORAGE_MANAGER_ROOT_PATH STORAGE_MANAGER_MOUNT_POINT "/geolocator"
#define STORAGE_MANAGER_OBJECTS_PATH STORAGE_MANAGER_ROOT_PATH "/objects"

bool storage_manager_is_ready(void);
/* The high-level operation guard (SD card and TFT share SPI2): these host tests have one task, nothing to serialize. */
static inline esp_err_t storage_manager_operation_begin(void) { return ESP_OK; }
static inline void storage_manager_operation_end(void) {}
esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size);
esp_err_t storage_manager_read_text_known_present(const char *path, char *buffer, size_t buffer_size);
esp_err_t storage_manager_atomic_write_text(const char *path, const char *text);
