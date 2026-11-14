#include "sd_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "sd_card_internal.h"
#include "sd_card_recovery_internal.h"


static const char *TAG = "SD_CARD";

static const esp_vfs_fat_mount_config_t s_mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 32U * 1024U,
    .disk_status_check_enable = false,
};

typedef struct {
    sdmmc_card_t *card;
    sdmmc_host_t host;
    int device_handle;
    BYTE pdrv;
    FATFS *fs;
    bool device_initialized;
    bool diskio_registered;
    bool vfs_registered;
    bool filesystem_mounted;
} sd_card_recovery_context_t;

static sd_card_recovery_context_t s_context = {
    .device_handle = -1,
    .pdrv = FF_DRV_NOT_USED,
};


static void sd_card_recovery_set_report(
    sd_card_mount_report_t *report,
    sd_card_mount_stage_t stage,
    esp_err_t err,
    int fresult,
    sd_card_health_t health
)
{
    if (report == NULL) {
        return;
    }

    report->stage = stage;
    report->err = err;
    report->fresult = fresult;
    report->health = health;
}


static esp_err_t sd_card_recovery_device_deinit(void)
{
    if (!s_context.device_initialized) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    if ((s_context.host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) != 0U) {
        if (s_context.host.deinit_p != NULL) {
            err = s_context.host.deinit_p(s_context.device_handle);
        }
    } else if (s_context.host.deinit != NULL) {
        err = s_context.host.deinit();
    }

    s_context.device_initialized = false;
    s_context.device_handle = -1;
    return err;
}


static esp_err_t sd_card_recovery_unmount_owned(void)
{
    const bool owns_card = s_context.card != NULL;
    if (!owns_card) {
        return ESP_OK;
    }

    sd_card_internal_set_card(NULL);

    esp_err_t cleanup_err = ESP_OK;
    if (s_context.pdrv != FF_DRV_NOT_USED) {
        char drive[3] = {
            (char)('0' + s_context.pdrv),
            ':',
            '\0',
        };
        if (f_mount(NULL, drive, 0U) != FR_OK) {
            cleanup_err = ESP_FAIL;
        }
    }
    s_context.filesystem_mounted = false;

    if (s_context.vfs_registered) {
        const esp_err_t err = esp_vfs_fat_unregister_path(sd_card_internal_mount_point());
        if (cleanup_err == ESP_OK && err != ESP_OK) {
            cleanup_err = err;
        }
        s_context.vfs_registered = false;
        s_context.fs = NULL;
    }

    if (s_context.diskio_registered) {
        ff_diskio_unregister(s_context.pdrv);
        s_context.diskio_registered = false;
    }

    const esp_err_t device_err = sd_card_recovery_device_deinit();
    if (cleanup_err == ESP_OK && device_err != ESP_OK) {
        cleanup_err = device_err;
    }
    free(s_context.card);

    memset(&s_context, 0, sizeof(s_context));
    s_context.device_handle = -1;
    s_context.pdrv = FF_DRV_NOT_USED;
    sd_card_internal_set_recovery_unmount(NULL);
    return cleanup_err;
}


static esp_err_t sd_card_recovery_mount_filesystem_impl(int *out_fresult)
{
    if (out_fresult != NULL) {
        *out_fresult = -1;
    }
    if (s_context.card == NULL || s_context.pdrv == FF_DRV_NOT_USED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_context.filesystem_mounted) {
        return ESP_OK;
    }

    char drive[3] = {
        (char)('0' + s_context.pdrv),
        ':',
        '\0',
    };
    const esp_vfs_fat_conf_t vfs_config = {
        .base_path = sd_card_internal_mount_point(),
        .fat_drive = drive,
        .max_files = (size_t)s_mount_config.max_files,
    };

    esp_err_t err = esp_vfs_fat_register_cfg(&vfs_config, &s_context.fs);
    if (err != ESP_OK) {
        s_context.fs = NULL;
        return err;
    }
    s_context.vfs_registered = true;

    const FRESULT fresult = f_mount(s_context.fs, drive, 1U);
    if (out_fresult != NULL) {
        *out_fresult = (int)fresult;
    }
    if (fresult != FR_OK) {
        (void)f_mount(NULL, drive, 0U);
        (void)esp_vfs_fat_unregister_path(sd_card_internal_mount_point());
        s_context.fs = NULL;
        s_context.vfs_registered = false;
        return ESP_FAIL;
    }

    s_context.filesystem_mounted = true;
    return ESP_OK;
}


static esp_err_t sd_card_recovery_initialize_card(
    sd_card_mount_report_t *report
)
{
    esp_err_t err = sd_card_internal_prepare_shared_spi();
    if (err != ESP_OK) {
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_HOST_INIT,
            err,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return err;
    }

    const sdmmc_host_t default_host = SDSPI_HOST_DEFAULT();
    s_context.host = default_host;
    s_context.host.slot = BOARD_SPI_HOST;
    s_context.host.max_freq_khz = SD_CARD_SPI_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = BOARD_SPI_HOST;
    slot_config.gpio_cs = BOARD_SD_CS_GPIO;

    s_context.card = calloc(1U, sizeof(*s_context.card));
    if (s_context.card == NULL) {
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_HOST_INIT,
            ESP_ERR_NO_MEM,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return ESP_ERR_NO_MEM;
    }

    if (s_context.host.init == NULL) {
        (void)sd_card_recovery_unmount_owned();
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_HOST_INIT,
            ESP_ERR_INVALID_STATE,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return ESP_ERR_INVALID_STATE;
    }

    err = s_context.host.init();
    if (err != ESP_OK) {
        (void)sd_card_recovery_unmount_owned();
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_HOST_INIT,
            err,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return err;
    }
    err = sdspi_host_init_device(&slot_config, &s_context.device_handle);
    if (err != ESP_OK) {
        (void)sd_card_recovery_unmount_owned();
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_HOST_INIT,
            err,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return err;
    }
    s_context.device_initialized = true;
    s_context.host.slot = s_context.device_handle;

    err = sdmmc_card_init(&s_context.host, s_context.card);
    if (err != ESP_OK) {
        (void)sd_card_recovery_unmount_owned();
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_CARD_INIT,
            err,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return err;
    }

    err = ff_diskio_get_drive(&s_context.pdrv);
    if (err != ESP_OK || s_context.pdrv == FF_DRV_NOT_USED) {
        const esp_err_t drive_err = err == ESP_OK ? ESP_ERR_NO_MEM : err;
        sdmmc_card_t *card = s_context.card;
        s_context.card = NULL;
        (void)sd_card_recovery_device_deinit();
        free(card);
        memset(&s_context, 0, sizeof(s_context));
        s_context.device_handle = -1;
        s_context.pdrv = FF_DRV_NOT_USED;
        sd_card_recovery_set_report(
            report,
            SD_CARD_MOUNT_STAGE_HOST_INIT,
            drive_err,
            -1,
            SD_CARD_HEALTH_CARD_UNAVAILABLE
        );
        return drive_err;
    }

    ff_diskio_register_sdmmc(s_context.pdrv, s_context.card);
    ff_sdmmc_set_disk_status_check(s_context.pdrv, s_mount_config.disk_status_check_enable);
    s_context.diskio_registered = true;
    sd_card_internal_set_card(s_context.card);
    sd_card_internal_set_recovery_unmount(sd_card_recovery_unmount_owned);

    return ESP_OK;
}


esp_err_t sd_card_mount_recovery(sd_card_mount_report_t *report)
{
    sd_card_mount_report_t local_report = {
        .stage = SD_CARD_MOUNT_STAGE_NONE,
        .err = ESP_OK,
        .fresult = -1,
        .health = SD_CARD_HEALTH_UNKNOWN,
    };
    if (report == NULL) {
        report = &local_report;
    }

#if defined(CONFIG_APP_TEST_RECORDING_SCENARIO) && CONFIG_APP_TEST_RECORDING_SCENARIO
    /* ESP-EMU does not model SDSPI: a flash FAT volume stands in for the card (sd_card_test_flash.c). */
    {
        const esp_err_t test_err =
            sd_card_internal_card() != NULL ? ESP_OK : sd_card_test_flash_mount();
        sd_card_recovery_set_report(
            report,
            test_err == ESP_OK ? SD_CARD_MOUNT_STAGE_NONE : SD_CARD_MOUNT_STAGE_FS_MOUNT,
            test_err,
            test_err == ESP_OK ? 0 : -1,
            test_err == ESP_OK ? SD_CARD_HEALTH_READY : SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE
        );
        return test_err;
    }
#endif

    if (s_context.card == NULL) {
        if (sd_card_internal_card() != NULL) {
            sd_card_recovery_set_report(
                report,
                SD_CARD_MOUNT_STAGE_HOST_INIT,
                ESP_ERR_INVALID_STATE,
                -1,
                SD_CARD_HEALTH_CARD_UNAVAILABLE
            );
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t err = sd_card_recovery_initialize_card(report);
        if (err != ESP_OK) {
            return err;
        }

        ESP_LOGI(
            TAG,
            "Recovery mount initialized SD over shared SPI2 at %.2f MHz",
            (double)s_context.host.max_freq_khz / 1000.0
        );
        sdmmc_card_print_info(stdout, s_context.card);
    }

    int fresult = -1;
    const esp_err_t err = sd_card_recovery_mount_filesystem_impl(&fresult);
    sd_card_recovery_set_report(
        report,
        err == ESP_OK ? SD_CARD_MOUNT_STAGE_NONE : SD_CARD_MOUNT_STAGE_FS_MOUNT,
        err,
        fresult,
        err == ESP_OK ? SD_CARD_HEALTH_READY : SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE
    );
    return err;
}


bool sd_card_recovery_owns_card(const sdmmc_card_t *card)
{
    return card != NULL && card == s_context.card;
}


int sd_card_recovery_drive_number(void)
{
    return s_context.pdrv == FF_DRV_NOT_USED ? -1 : (int)s_context.pdrv;
}


esp_err_t sd_card_recovery_prepare_for_format(void)
{
    if (s_context.card == NULL || s_context.pdrv == FF_DRV_NOT_USED ||
        !s_context.diskio_registered) {
        return ESP_ERR_INVALID_STATE;
    }

    char drive[3] = {
        (char)('0' + s_context.pdrv),
        ':',
        '\0',
    };
    if (s_context.filesystem_mounted || s_context.fs != NULL) {
        const FRESULT unmount_result = f_mount(NULL, drive, 0U);
        if (unmount_result != FR_OK) {
            return ESP_FAIL;
        }
        s_context.filesystem_mounted = false;
    }
    if (s_context.vfs_registered) {
        const esp_err_t err = esp_vfs_fat_unregister_path(sd_card_internal_mount_point());
        if (err != ESP_OK) {
            return err;
        }
        s_context.vfs_registered = false;
        s_context.fs = NULL;
    }
    ff_diskio_register_sdmmc(s_context.pdrv, s_context.card);
    ff_sdmmc_set_disk_status_check(s_context.pdrv, false);
    return ESP_OK;
}


void sd_card_recovery_restore_default_diskio(void)
{
    if (s_context.card == NULL || s_context.pdrv == FF_DRV_NOT_USED) {
        return;
    }
    ff_diskio_register_sdmmc(s_context.pdrv, s_context.card);
    ff_sdmmc_set_disk_status_check(s_context.pdrv, s_mount_config.disk_status_check_enable);
}


esp_err_t sd_card_recovery_mount_filesystem(int *out_fresult)
{
    sd_card_recovery_restore_default_diskio();
    return sd_card_recovery_mount_filesystem_impl(out_fresult);
}
