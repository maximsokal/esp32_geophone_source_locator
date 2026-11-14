/*
 * TEST ONLY (CONFIG_APP_TEST_RECORDING_SCENARIO): a FAT volume on a flash partition stands in for the SD card so the
 * recording path can run in ESP-EMU, which does not model SDSPI. Every consumer sees a mounted card at the usual mount
 * point; nothing talks to the (fake) sdmmc_card_t.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "sd_card_internal.h"

static const char *TAG = "SD_CARD_TEST";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static sdmmc_card_t s_fake_card;

static esp_err_t sd_card_test_flash_unmount(void)
{
    sd_card_internal_set_card(NULL);
    const esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(sd_card_internal_mount_point(), s_wl);
    s_wl = WL_INVALID_HANDLE;
    return err;
}

esp_err_t sd_card_test_flash_mount(void)
{
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
    };
    const esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        sd_card_internal_mount_point(), "storage_test", &config, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TEST_FLASH_STORAGE mount failed: %s", esp_err_to_name(err));
        return err;
    }
    memset(&s_fake_card, 0, sizeof(s_fake_card));
    s_fake_card.csd.sector_size = 512;
    s_fake_card.csd.capacity = (int)(wl_size(s_wl) / 512U);
    sd_card_internal_set_card(&s_fake_card);
    sd_card_internal_set_recovery_unmount(sd_card_test_flash_unmount);
    ESP_LOGW(TAG, "TEST_FLASH_STORAGE mounted at %s (%u KiB), not an SD card", sd_card_internal_mount_point(),
             (unsigned)(wl_size(s_wl) / 1024U));
    return ESP_OK;
}
