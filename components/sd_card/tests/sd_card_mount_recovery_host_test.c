#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sd_card.h"
#include "sd_card_internal.h"


static sdmmc_card_t s_card;
static FATFS s_fs;
static esp_err_t s_host_init_result;
static esp_err_t s_device_init_result;
static esp_err_t s_card_init_result;
static esp_err_t s_drive_result;
static esp_err_t s_vfs_register_result;
static FRESULT s_mount_script[4];
static unsigned s_mount_script_count;
static unsigned s_mount_script_index;
static unsigned s_prepare_spi_calls;
static unsigned s_host_init_calls;
static unsigned s_device_init_calls;
static unsigned s_card_init_calls;
static unsigned s_mount_calls;
static unsigned s_unmount_calls;
static unsigned s_vfs_register_calls;
static unsigned s_vfs_unregister_calls;
static unsigned s_disk_register_calls;
static unsigned s_disk_unregister_calls;
static unsigned s_device_remove_calls;
static unsigned s_print_calls;
static sdmmc_card_t *s_registered_card;


static void reset_fakes(void)
{
    s_host_init_result = ESP_OK;
    s_device_init_result = ESP_OK;
    s_card_init_result = ESP_OK;
    s_drive_result = ESP_OK;
    s_vfs_register_result = ESP_OK;
    memset(s_mount_script, 0, sizeof(s_mount_script));
    s_mount_script_count = 0U;
    s_mount_script_index = 0U;
    s_prepare_spi_calls = 0U;
    s_host_init_calls = 0U;
    s_device_init_calls = 0U;
    s_card_init_calls = 0U;
    s_mount_calls = 0U;
    s_unmount_calls = 0U;
    s_vfs_register_calls = 0U;
    s_vfs_unregister_calls = 0U;
    s_disk_register_calls = 0U;
    s_disk_unregister_calls = 0U;
    s_device_remove_calls = 0U;
    s_print_calls = 0U;
    s_registered_card = NULL;
    memset(&s_card, 0, sizeof(s_card));
    memset(&s_fs, 0, sizeof(s_fs));
    sd_card_internal_set_card(NULL);
    sd_card_internal_set_recovery_unmount(NULL);
}


const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_UNKNOWN";
    }
}


esp_err_t sd_card_internal_prepare_shared_spi(void)
{
    ++s_prepare_spi_calls;
    return ESP_OK;
}


esp_err_t fake_sdspi_host_init(void)
{
    ++s_host_init_calls;
    return s_host_init_result;
}


esp_err_t sdspi_host_init_device(const sdspi_device_config_t *config, int *out_handle)
{
    ++s_device_init_calls;
    assert(config != NULL);
    assert(config->host_id == BOARD_SPI_HOST);
    assert(config->gpio_cs == BOARD_SD_CS_GPIO);
    assert(out_handle != NULL);
    if (s_device_init_result == ESP_OK) {
        *out_handle = 7;
    }
    return s_device_init_result;
}


esp_err_t sdspi_host_remove_device(int slot)
{
    ++s_device_remove_calls;
    assert(slot == 7);
    return ESP_OK;
}


esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card)
{
    ++s_card_init_calls;
    assert(host != NULL);
    assert(host->slot == 7);
    assert(host->max_freq_khz == SDMMC_FREQ_DEFAULT);
    assert(card != NULL);
    if (s_card_init_result == ESP_OK) {
        card->csd.capacity = 16384U;
        card->csd.sector_size = 512U;
        card->max_freq_khz = SDMMC_FREQ_DEFAULT;
        card->real_freq_khz = SDMMC_FREQ_DEFAULT;
    }
    return s_card_init_result;
}


void sdmmc_card_print_info(FILE *stream, const sdmmc_card_t *card)
{
    ++s_print_calls;
    assert(stream == stdout);
    assert(card != NULL);
}


esp_err_t ff_diskio_get_drive(BYTE *out_pdrv)
{
    assert(out_pdrv != NULL);
    if (s_drive_result == ESP_OK) {
        *out_pdrv = 0U;
    }
    return s_drive_result;
}


void ff_diskio_register_sdmmc(unsigned char pdrv, sdmmc_card_t *card)
{
    ++s_disk_register_calls;
    assert(pdrv == 0U);
    assert(card != NULL);
    s_registered_card = card;
}


void ff_sdmmc_set_disk_status_check(BYTE pdrv, bool enable)
{
    assert(pdrv == 0U);
    assert(!enable);
}


void ff_diskio_unregister(BYTE pdrv)
{
    ++s_disk_unregister_calls;
    assert(pdrv == 0U);
}


esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *config, FATFS **out_fs)
{
    ++s_vfs_register_calls;
    assert(config != NULL);
    assert(strcmp(config->base_path, "/sdcard") == 0);
    assert(strcmp(config->fat_drive, "0:") == 0);
    assert(config->max_files == 5U);
    assert(out_fs != NULL);
    if (s_vfs_register_result == ESP_OK) {
        *out_fs = &s_fs;
    }
    return s_vfs_register_result;
}


esp_err_t esp_vfs_fat_unregister_path(const char *base_path)
{
    ++s_vfs_unregister_calls;
    assert(strcmp(base_path, "/sdcard") == 0);
    return ESP_OK;
}


FRESULT f_mount(FATFS *fs, const char *path, BYTE opt)
{
    assert(strcmp(path, "0:") == 0);
    if (fs == NULL) {
        ++s_unmount_calls;
        assert(opt == 0U);
        return FR_OK;
    }
    ++s_mount_calls;
    assert(fs == &s_fs);
    assert(opt == 1U);
    if (s_mount_script_index < s_mount_script_count) {
        return s_mount_script[s_mount_script_index++];
    }
    return FR_OK;
}


static esp_err_t mount_reported_fresult(FRESULT fresult, int expected)
{
    reset_fakes();
    s_mount_script[0] = fresult;
    s_mount_script_count = 1U;

    sd_card_mount_report_t report;
    const esp_err_t err = sd_card_mount_recovery(&report);

    assert(err == ESP_FAIL);
    assert(report.stage == SD_CARD_MOUNT_STAGE_FS_MOUNT);
    assert(report.err == ESP_FAIL);
    assert(report.fresult == expected);
    assert(report.health == SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE);
    assert(sd_card_internal_card() != NULL);
    assert(s_registered_card == sd_card_internal_card());
    assert(sd_card_internal_recovery_unmount() != NULL);
    assert(s_disk_unregister_calls == 0U);
    assert(s_vfs_unregister_calls == 1U);
    return ESP_OK;
}


static void test_fs_failure_keeps_card_and_retry_can_mount(void)
{
    assert(mount_reported_fresult(FR_NO_FILESYSTEM, 13) == ESP_OK);
    s_mount_script[1] = FR_OK;
    s_mount_script_count = 2U;

    sd_card_mount_report_t report;
    assert(sd_card_mount_recovery(&report) == ESP_OK);
    assert(report.stage == SD_CARD_MOUNT_STAGE_NONE);
    assert(report.health == SD_CARD_HEALTH_READY);
    assert(s_host_init_calls == 1U);
    assert(s_device_init_calls == 1U);
    assert(s_card_init_calls == 1U);
    assert(s_mount_calls == 2U);
    assert(sd_card_internal_recovery_unmount()() == ESP_OK);
    assert(sd_card_internal_card() == NULL);
    assert(s_disk_unregister_calls == 1U);
    assert(s_device_remove_calls == 1U);
}


static void test_fr_int_err_is_preserved(void)
{
    assert(mount_reported_fresult(FR_INT_ERR, 2) == ESP_OK);
    assert(sd_card_internal_recovery_unmount()() == ESP_OK);
}


static void test_card_init_timeout_does_not_keep_uninitialised_card(void)
{
    reset_fakes();
    s_card_init_result = ESP_ERR_TIMEOUT;
    sd_card_mount_report_t report;
    assert(sd_card_mount_recovery(&report) == ESP_ERR_TIMEOUT);
    assert(report.stage == SD_CARD_MOUNT_STAGE_CARD_INIT);
    assert(report.err == ESP_ERR_TIMEOUT);
    assert(report.fresult == -1);
    assert(report.health == SD_CARD_HEALTH_CARD_UNAVAILABLE);
    assert(sd_card_internal_card() == NULL);
    assert(s_disk_register_calls == 0U);
    assert(s_device_remove_calls == 1U);
}


static void test_host_init_error_is_reported_before_card_init(void)
{
    reset_fakes();
    s_host_init_result = ESP_FAIL;
    sd_card_mount_report_t report;
    assert(sd_card_mount_recovery(&report) == ESP_FAIL);
    assert(report.stage == SD_CARD_MOUNT_STAGE_HOST_INIT);
    assert(report.err == ESP_FAIL);
    assert(report.fresult == -1);
    assert(sd_card_internal_card() == NULL);
    assert(s_device_init_calls == 0U);
    assert(s_card_init_calls == 0U);
}


int main(void)
{
    test_fs_failure_keeps_card_and_retry_can_mount();
    test_fr_int_err_is_preserved();
    test_card_init_timeout_does_not_keep_uninitialised_card();
    test_host_init_error_is_reported_before_card_init();
    puts("SD_CARD_MOUNT_RECOVERY_HOST_PASS");
    return 0;
}
