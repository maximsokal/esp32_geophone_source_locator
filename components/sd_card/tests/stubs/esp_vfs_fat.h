#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"


typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
    bool disk_status_check_enable;
} esp_vfs_fat_mount_config_t;

esp_err_t esp_vfs_fat_sdspi_mount(
    const char *base_path,
    const sdmmc_host_t *host_config,
    const sdspi_device_config_t *slot_config,
    const esp_vfs_fat_mount_config_t *mount_config,
    sdmmc_card_t **out_card
);
