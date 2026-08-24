#include "sd_card.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"


static const char *TAG =
    "SD_CARD";


static const char *MOUNT_POINT =
    "/sdcard";


#define SD_CARD_SPI_FREQ_KHZ 2000


static sdmmc_card_t *s_card =
    NULL;


static void sd_card_make_uppercase(
    char *text
)
{
    while (*text != '\0') {
        *text = (char)toupper(
            (unsigned char)*text
        );

        text++;
    }
}


static const char *sd_card_type_name(void)
{
    if (s_card->is_sdio) {
        return "SDIO";
    }

    if (s_card->is_mmc) {
        return "MMC";
    }

    uint64_t capacity =
        (uint64_t)s_card->csd.capacity *
        (uint64_t)s_card->csd.sector_size;

    if (
        capacity >
        2ULL * 1024ULL *
        1024ULL * 1024ULL
    ) {
        return "SDHC/SDXC";
    }

    return "SDSC";
}


esp_err_t sd_card_mount(void)
{
    if (s_card != NULL) {
        ESP_LOGW(
            TAG,
            "SD card is already mounted"
        );

        return ESP_OK;
    }

    /*
     * TFT and SD share SPI2.
     * Keep TFT deselected before any SD transaction.
     */
    const gpio_config_t tft_cs_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_TFT_CS_GPIO,

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &tft_cs_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure TFT CS: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        gpio_set_level(
            BOARD_TFT_CS_GPIO,
            1
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set TFT CS HIGH: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "TFT CS forced HIGH"
    );

    ESP_LOGI(
        TAG,
        "Mounting SD card at 2.00 MHz"
    );

    sdmmc_host_t host =
        SDSPI_HOST_DEFAULT();

    host.slot =
        BOARD_SPI_HOST;

    host.max_freq_khz =
        SD_CARD_SPI_FREQ_KHZ;

    sdspi_device_config_t slot_config =
        SDSPI_DEVICE_CONFIG_DEFAULT();

    slot_config.host_id =
        BOARD_SPI_HOST;

    slot_config.gpio_cs =
        BOARD_SD_CS_GPIO;

    const esp_vfs_fat_mount_config_t
        mount_config = {
            .format_if_mount_failed =
                false,

            .max_files =
                5,

            .allocation_unit_size =
                32 * 1024,

            .disk_status_check_enable =
                false,
        };

    err =
        esp_vfs_fat_sdspi_mount(
            MOUNT_POINT,
            &host,
            &slot_config,
            &mount_config,
            &s_card
        );

    if (err != ESP_OK) {
        s_card =
            NULL;

        ESP_LOGE(
            TAG,
            "SD mount failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "SD card mounted successfully"
    );

    sdmmc_card_print_info(
        stdout,
        s_card
    );

    return ESP_OK;
}


esp_err_t sd_card_test(void)
{
    if (s_card == NULL) {
        ESP_LOGE(
            TAG,
            "SD card is not mounted"
        );

        return ESP_ERR_INVALID_STATE;
    }

    const char *file_path =
        "/sdcard/test.txt";

    const char *test_text =
        "ESP32 Geophone Source Locator\r\n"
        "SD card read/write test OK\r\n";

    ESP_LOGI(
        TAG,
        "Starting isolated SD write test"
    );

    ESP_LOGI(
        TAG,
        "Opening %s for writing",
        file_path
    );

    FILE *file =
        fopen(
            file_path,
            "w"
        );

    if (file == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to open file for writing"
        );

        return ESP_FAIL;
    }

    int written =
        fprintf(
            file,
            "%s",
            test_text
        );

    if (written < 0) {
        ESP_LOGE(
            TAG,
            "Failed to write test data"
        );

        fclose(
            file
        );

        return ESP_FAIL;
    }

    if (
        fflush(
            file
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to flush test file"
        );

        fclose(
            file
        );

        return ESP_FAIL;
    }

    if (
        fclose(
            file
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to close test file"
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Write test successful"
    );

    ESP_LOGI(
        TAG,
        "Opening %s for reading",
        file_path
    );

    file =
        fopen(
            file_path,
            "r"
        );

    if (file == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to open test file for reading"
        );

        return ESP_FAIL;
    }

    char buffer[128] = {0};

    size_t bytes_read =
        fread(
            buffer,
            1,
            sizeof(buffer) - 1,
            file
        );

    if (
        ferror(
            file
        )
    ) {
        ESP_LOGE(
            TAG,
            "Failed to read test file"
        );

        fclose(
            file
        );

        return ESP_FAIL;
    }

    if (
        fclose(
            file
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to close test file after reading"
        );

        return ESP_FAIL;
    }

    buffer[bytes_read] =
        '\0';

    ESP_LOGI(
        TAG,
        "Read %u bytes:",
        (unsigned)bytes_read
    );

    ESP_LOGI(
        TAG,
        "%s",
        buffer
    );

    if (
        strcmp(
            buffer,
            test_text
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Read-back verification failed"
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Read-back verification successful"
    );

    ESP_LOGI(
        TAG,
        "Isolated SD test passed"
    );

    return ESP_OK;
}


esp_err_t sd_card_get_info(
    sd_card_info_t *info
)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(
        info,
        0,
        sizeof(*info)
    );

    snprintf(
        info->name,
        sizeof(info->name),
        "%s",
        s_card->cid.name
    );

    sd_card_make_uppercase(
        info->name
    );

    snprintf(
        info->type,
        sizeof(info->type),
        "%s",
        sd_card_type_name()
    );

    info->capacity_bytes =
        (uint64_t)s_card->csd.capacity *
        (uint64_t)s_card->csd.sector_size;

    info->speed_khz =
        s_card->real_freq_khz > 0
            ? (uint32_t)s_card->real_freq_khz
            : s_card->max_freq_khz;

    return esp_vfs_fat_info(
        MOUNT_POINT,
        &info->total_bytes,
        &info->free_bytes
    );
}


esp_err_t sd_card_format(void)
{
    /*
     * Full FAT formatting is intentionally disabled.
     *
     * The device uses ERASE DATA to clear only its own
     * /sdcard/geolocator directory.
     */
    ESP_LOGE(
        TAG,
        "Full SD formatting is disabled"
    );

    return ESP_ERR_NOT_SUPPORTED;
}


bool sd_card_is_mounted(void)
{
    if (s_card == NULL) {
        return false;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    return
        esp_vfs_fat_info(
            MOUNT_POINT,
            &total_bytes,
            &free_bytes
        ) == ESP_OK;
}


void sd_card_unmount(void)
{
    if (s_card == NULL) {
        return;
    }

    sdmmc_card_t *card =
        s_card;

    s_card =
        NULL;

    esp_err_t err =
        esp_vfs_fat_sdcard_unmount(
            MOUNT_POINT,
            card
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "SD unmount returned: %s",
            esp_err_to_name(err)
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "SD card unmounted"
    );
}