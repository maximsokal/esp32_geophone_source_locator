#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio_impl.h"
#include "app_stop_timeout_config.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "ff.h"
#include "ffconf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card_diagnostics.h"
#include "sd_card_format.h"
#include "sd_card_recovery_internal.h"


static sdmmc_card_t s_card;
static ff_diskio_impl_t s_diskio;
static bool s_diskio_registered;
static TaskFunction_t s_scheduled_task;
static BaseType_t s_task_create_result;
static unsigned s_task_create_calls;
static unsigned s_alloc_size;
static unsigned s_alloc_caps;
static unsigned s_yield_calls;
static unsigned s_write_calls;
static unsigned s_successful_sector_writes;
static unsigned s_restore_calls;
static unsigned s_mount_calls;
static unsigned s_probe_calls;
static unsigned s_watchdog_add_calls;
static unsigned s_watchdog_reset_calls;
static unsigned s_watchdog_delete_calls;
static unsigned s_task_delete_calls;
static bool s_run_scheduled_task_on_delay;
static unsigned s_progress_samples[8];
static unsigned s_progress_sample_count;
static unsigned s_fail_write_on_call;
static esp_err_t s_write_error;
static FRESULT s_fdisk_result;
static FRESULT s_mkfs_result;
static esp_err_t s_mount_error;
static int s_mount_fresult;
static sd_card_probe_result_t s_probe_result;
static bool s_fake_mounted;

static void run_scheduled_task(void);


static void reset_fakes(void)
{
    memset(&s_card, 0, sizeof(s_card));
    s_card.csd.capacity = 16384U;
    s_card.csd.sector_size = 512U;
    memset(&s_diskio, 0, sizeof(s_diskio));
    s_diskio_registered = false;
    s_scheduled_task = NULL;
    s_task_create_result = pdPASS;
    s_task_create_calls = 0U;
    s_alloc_size = 0U;
    s_alloc_caps = 0U;
    s_yield_calls = 0U;
    s_write_calls = 0U;
    s_successful_sector_writes = 0U;
    s_restore_calls = 0U;
    s_mount_calls = 0U;
    s_probe_calls = 0U;
    s_watchdog_add_calls = 0U;
    s_watchdog_reset_calls = 0U;
    s_watchdog_delete_calls = 0U;
    s_task_delete_calls = 0U;
    s_run_scheduled_task_on_delay = false;
    memset(s_progress_samples, 0, sizeof(s_progress_samples));
    s_progress_sample_count = 0U;
    s_fail_write_on_call = 0U;
    s_write_error = ESP_ERR_TIMEOUT;
    s_fdisk_result = FR_OK;
    s_mkfs_result = FR_OK;
    s_mount_error = ESP_OK;
    s_mount_fresult = FR_OK;
    s_probe_result = (sd_card_probe_result_t){
        .stage = SD_CARD_PROBE_STAGE_COMPLETE,
        .err = ESP_OK,
    };
    s_fake_mounted = false;
}


const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_UNKNOWN";
    }
}


void *heap_caps_malloc(size_t size, unsigned caps)
{
    s_alloc_size = (unsigned)size;
    s_alloc_caps = caps;
    return malloc(size);
}


void heap_caps_free(void *ptr)
{
    free(ptr);
}


BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task,
    const char *name,
    unsigned stack_depth,
    void *parameters,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id,
    UBaseType_t memory_caps
)
{
    (void)parameters;
    ++s_task_create_calls;
    assert(task != NULL);
    assert(strcmp(name, "sd_format") == 0);
    assert(stack_depth == 12288U);
    assert(priority == 2U);
    assert(core_id == 0);
    if (s_task_create_result == pdPASS) {
        s_scheduled_task = task;
        if (created_task != NULL) *created_task = (TaskHandle_t)(uintptr_t)1U;
    }
    s_alloc_caps = memory_caps;
    return s_task_create_result;
}


void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1U);
    ++s_yield_calls;
    if (s_run_scheduled_task_on_delay) {
        s_run_scheduled_task_on_delay = false;
        run_scheduled_task();
    }
}


void vTaskDelete(TaskHandle_t task)
{
    (void)task;
    ++s_task_delete_calls;
}


esp_err_t esp_task_wdt_add(void *task)
{
    (void)task;
    ++s_watchdog_add_calls;
    return ESP_OK;
}


esp_err_t esp_task_wdt_reset(void)
{
    ++s_watchdog_reset_calls;
    return ESP_OK;
}


esp_err_t esp_task_wdt_delete(void *task)
{
    (void)task;
    ++s_watchdog_delete_calls;
    return ESP_OK;
}


void ff_diskio_register(BYTE pdrv, const ff_diskio_impl_t *impl)
{
    assert(pdrv == 0U);
    if (impl == NULL) {
        memset(&s_diskio, 0, sizeof(s_diskio));
        s_diskio_registered = false;
        return;
    }
    s_diskio = *impl;
    s_diskio_registered = true;
}


esp_err_t sdmmc_get_status(sdmmc_card_t *card)
{
    assert(card == &s_card);
    return ESP_OK;
}


esp_err_t sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t sector, size_t count)
{
    (void)dst;
    (void)sector;
    (void)count;
    assert(card == &s_card);
    return ESP_OK;
}


esp_err_t sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t sector, size_t count)
{
    (void)src;
    (void)sector;
    ++s_write_calls;
    assert(card == &s_card);
    if (s_fail_write_on_call != 0U && s_write_calls == s_fail_write_on_call) {
        return s_write_error;
    }
    s_successful_sector_writes += (unsigned)count;
    return ESP_OK;
}


bool sd_card_recovery_owns_card(const sdmmc_card_t *card)
{
    return card == &s_card;
}


int sd_card_recovery_drive_number(void)
{
    return 0;
}


esp_err_t sd_card_recovery_prepare_for_format(void)
{
    s_fake_mounted = false;
    return ESP_OK;
}


void sd_card_recovery_restore_default_diskio(void)
{
    ++s_restore_calls;
}


esp_err_t sd_card_recovery_mount_filesystem(int *out_fresult)
{
    ++s_mount_calls;
    if (out_fresult != NULL) {
        *out_fresult = s_mount_fresult;
    }
    if (s_mount_error == ESP_OK) {
        s_fake_mounted = true;
    }
    return s_mount_error;
}


sdmmc_card_t *sd_card_get_card_handle(void)
{
    return &s_card;
}


sd_card_probe_result_t sd_card_write_readback_probe(void)
{
    ++s_probe_calls;
    return s_probe_result;
}


static void sample_progress(void)
{
    sd_card_format_status_t status;
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(s_progress_sample_count < sizeof(s_progress_samples) / sizeof(s_progress_samples[0]));
    s_progress_samples[s_progress_sample_count++] = status.percent;
}


FRESULT f_fdisk(BYTE pdrv, const LBA_t partition_list[], void *workbuf)
{
    static const BYTE write_data[512] = {0};
    assert(pdrv == 0U);
    assert(partition_list[0] == 100U);
    assert(workbuf != NULL);
    assert(s_diskio_registered);
    if (s_fdisk_result != FR_OK) {
        return s_fdisk_result;
    }
    if (s_diskio.write(pdrv, write_data, 0U, 1U) != RES_OK) {
        return FR_DISK_ERR;
    }
    sample_progress();
    return FR_OK;
}


FRESULT f_mkfs(const char *path, const MKFS_PARM *options, void *workbuf, UINT workbuf_size)
{
    static const BYTE write_data[512] = {0};
    assert(strcmp(path, "0:") == 0);
    assert(options != NULL);
    assert(options->fmt == FM_FAT32);
    assert(options->au_size == 32U * 1024U);
    assert(options->n_fat == 2U);
    assert(workbuf != NULL);
    assert(workbuf_size == 4096U);
    assert(s_diskio_registered);

    DWORD trim_range[2] = {0U, 16383U};
    assert(s_diskio.ioctl(0U, CTRL_TRIM, trim_range) == RES_PARERR);

    if (s_diskio.write(0U, write_data, 1U, 40U) != RES_OK) {
        return FR_DISK_ERR;
    }
    sample_progress();
    if (s_diskio.write(0U, write_data, 41U, 80U) != RES_OK) {
        return FR_DISK_ERR;
    }
    sample_progress();
    return s_mkfs_result;
}


static void run_scheduled_task(void)
{
    assert(s_scheduled_task != NULL);
    TaskFunction_t task = s_scheduled_task;
    s_scheduled_task = NULL;
    task(NULL);
}


static void test_start_requires_explicit_confirmation(void)
{
    reset_fakes();
    assert(sd_card_format_fat32_task_start(&s_card, false) == ESP_ERR_INVALID_ARG);
    assert(s_task_create_calls == 0U);
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);
    assert(s_task_create_calls == 1U);
    run_scheduled_task();
}


static void test_format_success_progress_and_yields(void)
{
    reset_fakes();
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);
    sd_card_format_status_t before;
    assert(sd_card_format_fat32_task_get_status(&before) == ESP_OK);
    assert(before.state == SD_CARD_FORMAT_STATE_RUNNING);
    assert(before.percent == 0U);

    run_scheduled_task();

    sd_card_format_status_t after;
    assert(sd_card_format_fat32_task_get_status(&after) == ESP_OK);
    assert(after.state == SD_CARD_FORMAT_STATE_DONE);
    assert(after.err == ESP_OK);
    assert(after.percent == 100U);
    assert(s_progress_sample_count == 3U);
    assert(s_progress_samples[0] <= s_progress_samples[1]);
    assert(s_progress_samples[1] <= s_progress_samples[2]);
    assert(s_progress_samples[2] < 100U);
    assert(s_yield_calls == s_write_calls);
    assert(s_yield_calls == 3U);
    assert(s_successful_sector_writes == 121U);
    assert(s_watchdog_add_calls == 1U);
    assert(s_watchdog_reset_calls == s_write_calls);
    assert(s_watchdog_delete_calls == 1U);
    assert(s_mount_calls == 1U);
    assert(s_probe_calls == 1U);
    assert(s_fake_mounted);
    assert(s_alloc_size == 4096U);
    assert((s_alloc_caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) ==
           (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    assert(s_restore_calls == 1U);
    assert(s_task_delete_calls == 1U);
}


static void test_mkfs_failure_is_failed_and_never_mounts(void)
{
    reset_fakes();
    s_mkfs_result = FR_DISK_ERR;
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);
    run_scheduled_task();

    sd_card_format_status_t status;
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(status.state == SD_CARD_FORMAT_STATE_FAILED);
    assert(status.err == ESP_FAIL);
    assert(status.fresult == FR_DISK_ERR);
    assert(s_mount_calls == 0U);
    assert(!s_fake_mounted);
    assert(s_restore_calls == 1U);
}


static void test_lost_card_error_is_preserved(void)
{
    reset_fakes();
    s_fail_write_on_call = 2U;
    s_write_error = ESP_ERR_TIMEOUT;
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);
    run_scheduled_task();

    sd_card_format_status_t status;
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(status.state == SD_CARD_FORMAT_STATE_FAILED);
    assert(status.err == ESP_ERR_TIMEOUT);
    assert(s_mount_calls == 0U);
    assert(!s_fake_mounted);
    assert(s_yield_calls == 1U);
}

static void test_stop_joins_cooperative_worker_before_return(void)
{
    reset_fakes();
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);
    s_run_scheduled_task_on_delay = true;

    assert(sd_card_format_fat32_task_stop() == ESP_OK);

    sd_card_format_status_t status;
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(status.state == SD_CARD_FORMAT_STATE_FAILED);
    assert(status.err == ESP_ERR_INVALID_STATE);
    assert(s_task_delete_calls == 1U);
}

static void test_stop_timeout_marks_incomplete_and_retains_worker(void)
{
    reset_fakes();
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);

    assert(sd_card_format_fat32_task_stop() == ESP_ERR_TIMEOUT);
    assert(s_yield_calls == pdMS_TO_TICKS(APP_STOP_TIMEOUT_FORMAT_MS));
    assert(s_task_delete_calls == 0U);

    sd_card_format_status_t status;
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(status.state == SD_CARD_FORMAT_STATE_FAILED);
    assert(status.err == ESP_ERR_TIMEOUT);
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_ERR_INVALID_STATE);

    run_scheduled_task();
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(status.state == SD_CARD_FORMAT_STATE_FAILED);
    assert(status.err == ESP_ERR_TIMEOUT);
    assert(s_task_delete_calls == 1U);
}


int main(void)
{
    test_start_requires_explicit_confirmation();
    test_format_success_progress_and_yields();
    test_mkfs_failure_is_failed_and_never_mounts();
    test_lost_card_error_is_preserved();
    test_stop_joins_cooperative_worker_before_return();
    test_stop_timeout_marks_incomplete_and_retains_worker();
    puts("SD_CARD_FORMAT_HOST_PASS");
    return 0;
}
