#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "ff.h"

typedef struct {
    const char *base_path;
    const char *fat_drive;
    size_t max_files;
} esp_vfs_fat_conf_t;

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
    bool disk_status_check_enable;
} esp_vfs_fat_mount_config_t;

esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *config, FATFS **out_fs);
esp_err_t esp_vfs_fat_unregister_path(const char *base_path);
