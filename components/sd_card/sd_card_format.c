#include "sd_card_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "app_task_config.h"
#include "diskio_impl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "ff.h"
#include "ffconf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "sd_card_diagnostics.h"
#include "sd_card.h"
#include "sd_card_internal.h"
#include "sd_card_recovery_internal.h"


#define SD_CARD_FORMAT_WORKBUF_SIZE  4096U
#define SD_CARD_FORMAT_PROGRESS_OVERHEAD_SECTORS  128U
#define SD_CARD_FORMAT_FAT32_SECTORS_PER_CLUSTER  64U
#define SD_CARD_FORMAT_YIELD_TICKS  1U

_Static_assert(
    SD_CARD_FORMAT_WORKBUF_SIZE >= FF_MAX_SS,
    "FAT32 format work buffer must be at least FF_MAX_SS"
);


static const char *TAG = "SD_FORMAT";

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static sd_card_format_status_t s_status = {
    .state = SD_CARD_FORMAT_STATE_IDLE,
    .percent = 0U,
    .err = ESP_OK,
    .fresult = -1,
};
static sdmmc_card_t *s_format_card;
static int s_format_pdrv = -1;
static uint64_t s_written_sectors;
static uint64_t s_expected_sectors;
static esp_err_t s_io_error = ESP_OK;
static bool s_stop_requested;
static bool s_watchdog_subscribed;


static void sd_card_format_set_failure(esp_err_t err, int fresult)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.state = SD_CARD_FORMAT_STATE_FAILED;
    s_status.err = err == ESP_OK ? ESP_FAIL : err;
    s_status.fresult = fresult;
    taskEXIT_CRITICAL(&s_status_lock);
}


static bool sd_card_format_is_stopping(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    const bool stopping = s_stop_requested;
    taskEXIT_CRITICAL(&s_status_lock);
    return stopping;
}


static void sd_card_format_remember_io_error(esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }
    taskENTER_CRITICAL(&s_status_lock);
    if (s_io_error == ESP_OK) {
        s_io_error = err;
    }
    taskEXIT_CRITICAL(&s_status_lock);
}


static uint8_t sd_card_format_progress_percent(void)
{
    if (s_expected_sectors == 0U) {
        return 0U;
    }
    uint64_t percent = (s_written_sectors * 100U) / s_expected_sectors;
    if (percent > 99U) {
        percent = 99U;
    }
    return (uint8_t)percent;
}


static DSTATUS sd_card_format_disk_initialize(BYTE pdrv)
{
    if (pdrv != (BYTE)s_format_pdrv || s_format_card == NULL) {
        sd_card_format_remember_io_error(ESP_ERR_INVALID_STATE);
        return STA_NOINIT;
    }
    const esp_err_t err = sdmmc_get_status(s_format_card);
    if (err != ESP_OK) {
        sd_card_format_remember_io_error(err);
        return STA_NOINIT;
    }
    return 0U;
}


static DSTATUS sd_card_format_disk_status(BYTE pdrv)
{
    return pdrv == (BYTE)s_format_pdrv && s_format_card != NULL ? 0U : STA_NOINIT;
}


static DRESULT sd_card_format_disk_read(
    BYTE pdrv,
    BYTE *buffer,
    uint32_t sector,
    unsigned count
)
{
    if (pdrv != (BYTE)s_format_pdrv || s_format_card == NULL) {
        sd_card_format_remember_io_error(ESP_ERR_INVALID_STATE);
        return RES_PARERR;
    }
    const esp_err_t err = sdmmc_read_sectors(s_format_card, buffer, sector, count);
    if (err != ESP_OK) {
        sd_card_format_remember_io_error(err);
        return RES_ERROR;
    }
    return RES_OK;
}


static DRESULT sd_card_format_disk_write(
    BYTE pdrv,
    const BYTE *buffer,
    uint32_t sector,
    unsigned count
)
{
    if (pdrv != (BYTE)s_format_pdrv || s_format_card == NULL) {
        sd_card_format_remember_io_error(ESP_ERR_INVALID_STATE);
        return RES_PARERR;
    }
    if (sd_card_format_is_stopping()) {
        sd_card_format_remember_io_error(ESP_ERR_INVALID_STATE);
        return RES_ERROR;
    }

    const esp_err_t err = sdmmc_write_sectors(s_format_card, buffer, sector, count);
    if (err != ESP_OK) {
        sd_card_format_remember_io_error(err);
        return RES_ERROR;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_written_sectors += count;
    const uint8_t progress = sd_card_format_progress_percent();
    if (progress > s_status.percent) {
        s_status.percent = progress;
    }
    taskEXIT_CRITICAL(&s_status_lock);

    /* One bounded scheduling point per successful block-write call. */
    vTaskDelay((TickType_t)SD_CARD_FORMAT_YIELD_TICKS);
    if (s_watchdog_subscribed) {
        (void)esp_task_wdt_reset();
    }
    return RES_OK;
}


static DRESULT sd_card_format_disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    if (pdrv != (BYTE)s_format_pdrv || s_format_card == NULL) {
        sd_card_format_remember_io_error(ESP_ERR_INVALID_STATE);
        return RES_PARERR;
    }

    switch (command) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(DWORD *)buffer = s_format_card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buffer = s_format_card->csd.sector_size;
            return RES_OK;
        case GET_BLOCK_SIZE:
            return RES_ERROR;
#if FF_USE_TRIM
        case CTRL_TRIM:
            /* FatFs ignores an unsupported trim request during f_mkfs. */
            return RES_PARERR;
#endif
        default:
            return RES_ERROR;
    }
}


static const ff_diskio_impl_t s_format_diskio = {
    .init = sd_card_format_disk_initialize,
    .status = sd_card_format_disk_status,
    .read = sd_card_format_disk_read,
    .write = sd_card_format_disk_write,
    .ioctl = sd_card_format_disk_ioctl,
};


static bool sd_card_format_watchdog_begin(void)
{
    const esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGW(TAG, "TWDT subscribe failed: %s", esp_err_to_name(err));
    return false;
}


static void sd_card_format_watchdog_end(bool subscribed)
{
    if (subscribed) {
        const esp_err_t err = esp_task_wdt_delete(NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TWDT unsubscribe failed: %s", esp_err_to_name(err));
        }
    }
}


static esp_err_t sd_card_format_io_failure(int fresult)
{
    taskENTER_CRITICAL(&s_status_lock);
    const esp_err_t io_error = s_io_error;
    taskEXIT_CRITICAL(&s_status_lock);
    if (sd_card_format_is_stopping()) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)fresult;
    return io_error == ESP_OK ? ESP_FAIL : io_error;
}


static void sd_card_format_task(void *argument)
{
    (void)argument;
    const bool subscribed = sd_card_format_watchdog_begin();
    s_watchdog_subscribed = subscribed;

    esp_err_t err = sd_card_recovery_prepare_for_format();
    int fresult = -1;
    void *workbuf = NULL;
    bool wrapper_registered = false;

    if (err != ESP_OK) {
        goto finish;
    }
    if (sd_card_format_is_stopping()) {
        err = ESP_ERR_INVALID_STATE;
        goto finish;
    }

    workbuf = heap_caps_malloc(
        SD_CARD_FORMAT_WORKBUF_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
    );
    if (workbuf == NULL) {
        err = ESP_ERR_NO_MEM;
        goto finish;
    }

    LBA_t partition_list[4] = {100U, 0U, 0U, 0U};
    MKFS_PARM options = {
        .fmt = FM_FAT32,
        .n_fat = 2U,
        .n_root = 0U,
        .align = 0U,
        .au_size = 32U * 1024U,
    };

    s_format_pdrv = sd_card_recovery_drive_number();
    s_format_card = sd_card_get_card_handle();
    if (s_format_pdrv < 0 || s_format_card == NULL ||
        !sd_card_recovery_owns_card(s_format_card)) {
        err = ESP_ERR_INVALID_STATE;
        goto finish;
    }

    /*
     * FAT32 stores one 4-byte entry per 32 KiB cluster. The estimate below
     * counts two FAT copies (card sectors / 4096), the 32 KiB root area, boot
     * and partition metadata, plus 128 sectors for fixed/backup structures.
     * Progress is an estimate and is capped at 99 until mount + readback pass.
     */
    const uint64_t total_sectors = s_format_card->csd.capacity;
    const uint64_t sectors_per_fat =
        (total_sectors + (uint64_t)(SD_CARD_FORMAT_FAT32_SECTORS_PER_CLUSTER * 128U - 1U)) /
        (uint64_t)(SD_CARD_FORMAT_FAT32_SECTORS_PER_CLUSTER * 128U);
    s_expected_sectors =
        (2U * sectors_per_fat) + SD_CARD_FORMAT_FAT32_SECTORS_PER_CLUSTER +
        SD_CARD_FORMAT_PROGRESS_OVERHEAD_SECTORS;
    s_written_sectors = 0U;
    s_io_error = ESP_OK;

    ff_diskio_register((BYTE)s_format_pdrv, &s_format_diskio);
    wrapper_registered = true;

    fresult = (int)f_fdisk((BYTE)s_format_pdrv, partition_list, workbuf);
    if (fresult != (int)FR_OK) {
        err = sd_card_format_io_failure(fresult);
        goto finish;
    }
    if (sd_card_format_is_stopping()) {
        err = ESP_ERR_INVALID_STATE;
        goto finish;
    }

    char drive[3] = {
        (char)('0' + s_format_pdrv),
        ':',
        '\0',
    };
    fresult = (int)f_mkfs(
        drive,
        &options,
        workbuf,
        SD_CARD_FORMAT_WORKBUF_SIZE
    );
    if (fresult != (int)FR_OK) {
        err = sd_card_format_io_failure(fresult);
        goto finish;
    }
    if (sd_card_format_is_stopping()) {
        err = ESP_ERR_INVALID_STATE;
        goto finish;
    }

    sd_card_recovery_restore_default_diskio();
    wrapper_registered = false;
    err = sd_card_recovery_mount_filesystem(&fresult);
    if (err != ESP_OK) {
        goto finish;
    }

    {
        const sd_card_probe_result_t probe = sd_card_write_readback_probe();
        if (probe.err != ESP_OK) {
            err = probe.err;
            fresult = -1;
            goto finish;
        }
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status.state = SD_CARD_FORMAT_STATE_DONE;
    s_status.percent = 100U;
    s_status.err = ESP_OK;
    s_status.fresult = (int)FR_OK;
    taskEXIT_CRITICAL(&s_status_lock);
    err = ESP_OK;

finish:
    if (wrapper_registered) {
        sd_card_recovery_restore_default_diskio();
    }
    if (workbuf != NULL) {
        heap_caps_free(workbuf);
    }
    s_format_card = NULL;
    s_format_pdrv = -1;
    s_watchdog_subscribed = false;
    sd_card_format_watchdog_end(subscribed);
    if (err != ESP_OK) {
        sd_card_format_set_failure(err, fresult);
    }
    vTaskDelete(NULL);
}


esp_err_t sd_card_format_fat32_task_start(
    sdmmc_card_t *card,
    bool confirmed
)
{
    if (!confirmed) {
        return ESP_ERR_INVALID_ARG;
    }
    if (card == NULL || !sd_card_recovery_owns_card(card)) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_status_lock);
    if (s_status.state == SD_CARD_FORMAT_STATE_RUNNING) {
        taskEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SD_CARD_FORMAT_STATE_RUNNING;
    s_status.percent = 0U;
    s_status.err = ESP_OK;
    s_status.fresult = -1;
    s_stop_requested = false;
    taskEXIT_CRITICAL(&s_status_lock);

    BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        sd_card_format_task,
        "sd_format",
        APP_TASK_STACK_SD_FORMAT,
        NULL,
        APP_TASK_PRIORITY_SD_FORMAT,
        NULL,
        APP_TASK_CORE_SERVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (task_result != pdPASS) {
        task_result = xTaskCreatePinnedToCoreWithCaps(
            sd_card_format_task,
            "sd_format",
            APP_TASK_STACK_SD_FORMAT,
            NULL,
            APP_TASK_PRIORITY_SD_FORMAT,
            NULL,
            APP_TASK_CORE_SERVICES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (task_result != pdPASS) {
        sd_card_format_set_failure(ESP_ERR_NO_MEM, -1);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}


esp_err_t sd_card_format_fat32_task_get_status(
    sd_card_format_status_t *out_status
)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}


esp_err_t sd_card_format_fat32_task_stop(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    if (s_status.state != SD_CARD_FORMAT_STATE_RUNNING) {
        taskEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = true;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
