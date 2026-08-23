#include "sd_card.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"

static const char *TAG = "SD_CARD";
static const char *MOUNT_POINT = "/sdcard";

static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_mount(void)
{
    if (s_card != NULL) {
        ESP_LOGW(TAG, "SD card is already mounted");
        return ESP_OK;
    }

    /*
     * TFT и microSD используют одну SPI-шину.
     * Перед инициализацией SD гарантированно отключаем TFT.
     */
    const gpio_config_t tft_cs_config = {
        .pin_bit_mask = 1ULL << BOARD_TFT_CS_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&tft_cs_config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure TFT CS: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = gpio_set_level(BOARD_TFT_CS_GPIO, 1);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set TFT CS HIGH: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(TAG, "TFT CS forced HIGH");
    ESP_LOGI(TAG, "Mounting SD card");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    host.slot = BOARD_SPI_HOST;

    /*
     * Для первого запуска используем умеренную частоту.
     * После стабильной работы можно увеличить.
     */
    host.max_freq_khz = 10000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();

    slot_config.host_id = BOARD_SPI_HOST;
    slot_config.gpio_cs = BOARD_SD_CS_GPIO;

    /*
     * ВРЕМЕННО true:
     * если карта 64 GB сейчас exFAT или имеет неподходящую
     * файловую систему, ESP32 попробует отформатировать её.
     *
     * После первого успешного форматирования заменить на false.
     */
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 32 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD mount/format failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");

    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

esp_err_t sd_card_test(void)
{
    if (s_card == NULL) {
        ESP_LOGE(TAG, "SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    const char *file_path = "/sdcard/test.txt";

    const char *test_text =
        "ESP32 Geophone Source Locator\r\n"
        "SD card read/write test OK\r\n";

    ESP_LOGI(TAG, "Opening %s for writing", file_path);

    FILE *file = fopen(file_path, "w");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }

    int written = fprintf(file, "%s", test_text);

    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write file");
        fclose(file);
        return ESP_FAIL;
    }

    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "fflush failed");
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);

    ESP_LOGI(TAG, "Write test successful");

    ESP_LOGI(TAG, "Opening %s for reading", file_path);

    file = fopen(file_path, "r");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }

    char buffer[128] = {0};

    size_t bytes_read = fread(
        buffer,
        1,
        sizeof(buffer) - 1,
        file
    );

    fclose(file);

    buffer[bytes_read] = '\0';

    ESP_LOGI(
        TAG,
        "Read %u bytes:",
        (unsigned)bytes_read
    );

    ESP_LOGI(TAG, "%s", buffer);

    if (strcmp(buffer, test_text) != 0) {
        ESP_LOGE(TAG, "Read-back verification failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Read-back verification successful");

    return ESP_OK;
}

void sd_card_unmount(void)
{
    if (s_card == NULL) {
        return;
    }

    esp_vfs_fat_sdcard_unmount(
        MOUNT_POINT,
        s_card
    );

    s_card = NULL;

    ESP_LOGI(TAG, "SD card unmounted");
}