#include "sd_card_diagnostics.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "sd_card.h"
#include "sd_card_diagnostics_internal.h"


static const char *TAG =
    "SD_CARD";


/* Keep diagnostic files valid in fresh configurations with FATFS_LFN_NONE. */
static const char *TEST_FILE_PATH =
    "/sdcard/SDTEST.TMP";


static const char *TEST_TEXT =
    "ESP32 Geophone Source Locator\r\n"
    "SD card read/write test OK\r\n";

static const char *BOOT_PROBE_PATH =
    "/sdcard/SDPROBE.TMP";

static const char *BOOT_PROBE_TEXT =
    "sd-probe-v1:0123456789abcdef\n";


static void sd_card_diagnostics_cleanup(void)
{
    if (
        remove(
            TEST_FILE_PATH
        ) != 0
    ) {
        ESP_LOGW(
            TAG,
            "Failed to remove temporary SD test file"
        );
    }
}


esp_err_t sd_card_diagnostics_run_write_read_test(void)
{
    if (!sd_card_is_mounted()) {
        ESP_LOGE(
            TAG,
            "SD card is not mounted"
        );

        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Starting optional SD write/read diagnostic"
    );

    esp_err_t err =
        sd_card_diagnostics_write_file(
            TEST_FILE_PATH,
            TEST_TEXT
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD diagnostic write failed"
        );

        sd_card_diagnostics_cleanup();

        return err;
    }

    char buffer[128] = {0};

    err =
        sd_card_diagnostics_read_file(
            TEST_FILE_PATH,
            buffer,
            sizeof(buffer)
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD diagnostic read failed"
        );

        sd_card_diagnostics_cleanup();

        return err;
    }

    if (
        strcmp(
            buffer,
            TEST_TEXT
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "SD diagnostic read-back verification failed"
        );

        sd_card_diagnostics_cleanup();

        return ESP_FAIL;
    }

    sd_card_diagnostics_cleanup();

    ESP_LOGI(
        TAG,
        "Optional SD write/read diagnostic passed"
    );

    return ESP_OK;
}


const char *sd_card_probe_stage_name(sd_card_probe_stage_t stage)
{
    switch (stage) {
        case SD_CARD_PROBE_STAGE_NONE: return "NONE";
        case SD_CARD_PROBE_STAGE_MOUNT_CHECK: return "MOUNT_CHECK";
        case SD_CARD_PROBE_STAGE_CREATE: return "CREATE";
        case SD_CARD_PROBE_STAGE_WRITE: return "WRITE";
        case SD_CARD_PROBE_STAGE_FLUSH: return "FLUSH";
        case SD_CARD_PROBE_STAGE_CLOSE: return "CLOSE";
        case SD_CARD_PROBE_STAGE_READ: return "READ";
        case SD_CARD_PROBE_STAGE_COMPARE: return "COMPARE";
        case SD_CARD_PROBE_STAGE_REMOVE: return "REMOVE";
        case SD_CARD_PROBE_STAGE_COMPLETE: return "COMPLETE";
        default: return "UNKNOWN";
    }
}


static void sd_card_probe_log_failure(const sd_card_probe_result_t *result)
{
    ESP_LOGE(
        TAG,
        "SD boot probe failed: stage=%s err=%s errno=%d (%s) path=%s",
        sd_card_probe_stage_name(result->stage),
        esp_err_to_name(result->err),
        result->sys_errno,
        result->sys_errno == 0 ? "not supplied" : strerror(result->sys_errno),
        BOOT_PROBE_PATH
    );
}


sd_card_probe_result_t sd_card_write_readback_probe(void)
{
    sd_card_probe_result_t result = {
        .stage = SD_CARD_PROBE_STAGE_NONE,
        .err = ESP_OK,
        .sys_errno = 0,
    };

    if (!sd_card_is_mounted()) {
        result.stage = SD_CARD_PROBE_STAGE_MOUNT_CHECK;
        result.err = ESP_ERR_INVALID_STATE;
        sd_card_probe_log_failure(&result);
        return result;
    }

    errno = 0;
    FILE *file = fopen(BOOT_PROBE_PATH, "wb");
    if (file == NULL) {
        result.stage = SD_CARD_PROBE_STAGE_CREATE;
        result.err = ESP_FAIL;
        result.sys_errno = errno;
        goto cleanup;
    }

    const size_t text_size = strlen(BOOT_PROBE_TEXT);
    errno = 0;
    if (fwrite(BOOT_PROBE_TEXT, 1U, text_size, file) != text_size) {
        result.stage = SD_CARD_PROBE_STAGE_WRITE;
        result.err = ESP_FAIL;
        result.sys_errno = errno;
        (void)fclose(file);
        file = NULL;
        goto cleanup;
    }

    errno = 0;
    if (fflush(file) != 0) {
        result.stage = SD_CARD_PROBE_STAGE_FLUSH;
        result.err = ESP_FAIL;
        result.sys_errno = errno;
        (void)fclose(file);
        file = NULL;
        goto cleanup;
    }

    errno = 0;
    if (fclose(file) != 0) {
        result.stage = SD_CARD_PROBE_STAGE_CLOSE;
        result.err = ESP_FAIL;
        result.sys_errno = errno;
        file = NULL;
        goto cleanup;
    }
    file = NULL;

    char buffer[64] = {0};
    errno = 0;
    file = fopen(BOOT_PROBE_PATH, "rb");
    if (file == NULL) {
        result.stage = SD_CARD_PROBE_STAGE_READ;
        result.err = ESP_FAIL;
        result.sys_errno = errno;
        goto cleanup;
    }

    errno = 0;
    const size_t bytes_read = fread(buffer, 1U, sizeof(buffer) - 1U, file);
    const int read_errno = errno;
    const bool read_failed = ferror(file) != 0;
    errno = 0;
    const bool close_failed = fclose(file) != 0;
    const int close_errno = errno;
    file = NULL;
    if (read_failed) {
        result.stage = SD_CARD_PROBE_STAGE_READ;
        result.err = ESP_FAIL;
        result.sys_errno = read_errno;
        goto cleanup;
    }
    if (close_failed) {
        result.stage = SD_CARD_PROBE_STAGE_CLOSE;
        result.err = ESP_FAIL;
        result.sys_errno = close_errno;
        goto cleanup;
    }

    if (bytes_read != text_size || memcmp(buffer, BOOT_PROBE_TEXT, text_size) != 0) {
        result.stage = SD_CARD_PROBE_STAGE_COMPARE;
        result.err = ESP_FAIL;
        goto cleanup;
    }

cleanup:
    if (file != NULL) {
        (void)fclose(file);
    }
    errno = 0;
    if (remove(BOOT_PROBE_PATH) != 0 && result.err == ESP_OK) {
        result.stage = SD_CARD_PROBE_STAGE_REMOVE;
        result.err = ESP_FAIL;
        result.sys_errno = errno;
    }
    if (result.err == ESP_OK) {
        result.stage = SD_CARD_PROBE_STAGE_COMPLETE;
    } else {
        sd_card_probe_log_failure(&result);
    }
    return result;
}
