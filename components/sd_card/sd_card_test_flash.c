/*
 * TEST ONLY (CONFIG_APP_TEST_RECORDING_SCENARIO): a FAT volume on a flash partition stands in for the SD card so the
 * recording path can run in ESP-EMU, which does not model SDSPI. Every consumer sees a mounted card at the usual mount
 * point; nothing talks to the (fake) sdmmc_card_t.
 *
 * The disk layer models what the real SDSPI driver needs from internal RAM: spi_master bounces every 512-byte block
 * through an internal DMA-capable buffer (setup_dma_priv_buffer). When none is available the transfer fails exactly as
 * on the board ("Failed to allocate priv RX buffer"), so DMA-heap exhaustion in the recording path shows up in ESP-EMU.
 */
#include <stdatomic.h>
#include <string.h>

#include "diskio_impl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "wear_levelling.h"

#include "sd_card_internal.h"

/*
 * What one SD operation needs from internal DMA RAM at the moment of a transfer. The board gives the calibration:
 * with the largest DMA block at 1856 bytes the operations ran, with 960 bytes the driver failed ("Failed to allocate
 * priv RX buffer", board log 2026-11-14 10:29). storage_manager sizes its own DMA reserve for the same need
 * (STORAGE_MANAGER_DMA_RESERVE_BYTES = 1536), so one transfer is modelled as one 1536-byte internal DMA buffer. The FAT
 * sector of the flash volume is the wear-levelling sector (4096); one buffer is requested per FAT sector.
 */
#define TEST_SD_DMA_BOUNCE_BYTES 1536U
#define TEST_SD_DMA_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

static const char *TAG = "SD_CARD_TEST";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static BYTE s_pdrv = FF_DRV_NOT_USED;
static size_t s_sector = 4096U;
static FATFS *s_fs;
static sdmmc_card_t s_fake_card;
static atomic_uint s_bounce_failures;
static atomic_uint s_bounce_min_largest = UINT32_MAX;

unsigned sd_card_test_flash_bounce_failures(void)
{
    return atomic_load(&s_bounce_failures);
}

/* The DMA bounce buffer the real driver would take for one block; false when the heap cannot give it. */
static bool bounce_acquire(void)
{
    void *bounce = heap_caps_malloc(TEST_SD_DMA_BOUNCE_BYTES, TEST_SD_DMA_CAPS);
    if (bounce == NULL) {
        const unsigned largest = (unsigned)heap_caps_get_largest_free_block(TEST_SD_DMA_CAPS);
        if (atomic_fetch_add(&s_bounce_failures, 1U) < 8U) {
            ESP_LOGE("spi_master", "setup_dma_priv_buffer: Failed to allocate priv RX buffer (need %u, largest DMA block %u, DMA free %u)",
                     (unsigned)TEST_SD_DMA_BOUNCE_BYTES, largest, (unsigned)heap_caps_get_free_size(TEST_SD_DMA_CAPS));
        }
        return false;
    }
    heap_caps_free(bounce);
    return true;
}

/*
 * Write latency of the board's SD card. 39 fwrite calls of ~250 frames (2 KB) from the board logs of 2026-11-14 18:45
 * and 21:34, in ms: median 48, mean 59, p90 130, max 154. The time is not the transfer (2 KB take ~1 ms at 20 MHz): it is
 * the card's busy time and the wait for the TFT to give up the shared SPI bus. It is therefore charged per write call,
 * whatever its size, and drawn from the measured values, so a recording rate that needs more write calls per second than
 * the card delivers shows up as a growing cache backlog in ESP-EMU. Off the board's SD card the model is an assumption.
 */
#if defined(CONFIG_APP_TEST_RECORDING_SD_WRITE_LATENCY) && CONFIG_APP_TEST_RECORDING_SD_WRITE_LATENCY
static const uint16_t s_board_write_ms[] = {
    21, 25, 26, 26, 27, 28, 29, 29, 29, 31, 33, 34, 35, 36, 38, 43, 44, 45, 47, 48, 49, 49, 50, 53, 59, 60, 60, 67, 68, 70,
    76, 79, 87, 90, 129, 130, 134, 153, 154,
};
static atomic_uint s_model_write_calls;
static atomic_uint s_model_write_delay_ms;
static uint32_t s_model_rng = 0x2545F491U;

static void model_write_latency(void)
{
    s_model_rng ^= s_model_rng << 13;
    s_model_rng ^= s_model_rng >> 17;
    s_model_rng ^= s_model_rng << 5;
    const uint32_t ms = s_board_write_ms[s_model_rng % (sizeof(s_board_write_ms) / sizeof(s_board_write_ms[0]))];
    atomic_fetch_add(&s_model_write_calls, 1U);
    atomic_fetch_add(&s_model_write_delay_ms, ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
}
#else
static atomic_uint s_model_write_calls;
static atomic_uint s_model_write_delay_ms;
static void model_write_latency(void) {}
#endif

void sd_card_test_flash_write_model(unsigned *calls, unsigned long long *delay_ms)
{
    if (calls != NULL) *calls = atomic_load(&s_model_write_calls);
    if (delay_ms != NULL) *delay_ms = (unsigned long long)atomic_load(&s_model_write_delay_ms);
}

static DSTATUS test_init(BYTE pdrv) { (void)pdrv; return 0; }
static DSTATUS test_status(BYTE pdrv) { (void)pdrv; return 0; }

static DRESULT test_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    for (UINT i = 0U; i < count; ++i) {
        if (!bounce_acquire()) return RES_ERROR;
        if (wl_read(s_wl, (size_t)(sector + i) * s_sector, buff + (size_t)i * s_sector, s_sector) != ESP_OK) return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT test_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    model_write_latency();
    for (UINT i = 0U; i < count; ++i) {
        if (!bounce_acquire()) return RES_ERROR;
        const size_t address = (size_t)(sector + i) * s_sector;
        if (wl_erase_range(s_wl, address, s_sector) != ESP_OK) return RES_ERROR;
        if (wl_write(s_wl, address, buff + (size_t)i * s_sector, s_sector) != ESP_OK) return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT test_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD *)buff) = (DWORD)(wl_size(s_wl) / s_sector);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)s_sector;
        return RES_OK;
    default:
        return RES_ERROR;
    }
}

static esp_err_t sd_card_test_flash_unmount(void)
{
    sd_card_internal_set_card(NULL);
    char drive[3] = {(char)('0' + s_pdrv), ':', 0};
    (void)f_mount(NULL, drive, 0);
    (void)esp_vfs_fat_unregister_path(sd_card_internal_mount_point());
    ff_diskio_unregister(s_pdrv);
    s_pdrv = FF_DRV_NOT_USED;
    s_fs = NULL;
    if (s_wl != WL_INVALID_HANDLE) (void)wl_unmount(s_wl);
    s_wl = WL_INVALID_HANDLE;
    return ESP_OK;
}

esp_err_t sd_card_test_flash_mount(void)
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage_test");
    if (partition == NULL) {
        ESP_LOGE(TAG, "TEST_FLASH_STORAGE: partition storage_test not found");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = wl_mount(partition, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TEST_FLASH_STORAGE wl_mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sector = wl_sector_size(s_wl);
    if (ff_diskio_get_drive(&s_pdrv) != ESP_OK) return ESP_ERR_NOT_FOUND;
    const ff_diskio_impl_t impl = {
        .init = test_init, .status = test_status, .read = test_read, .write = test_write, .ioctl = test_ioctl,
    };
    ff_diskio_register(s_pdrv, &impl);
    char drive[3] = {(char)('0' + s_pdrv), ':', 0};
    const esp_vfs_fat_conf_t conf = {
        .base_path = sd_card_internal_mount_point(), .fat_drive = drive, .max_files = 5,
    };
    err = esp_vfs_fat_register_cfg(&conf, &s_fs);
    if (err != ESP_OK) return err;
    FRESULT result = f_mount(s_fs, drive, 1);
    if (result != FR_OK) {
        /* A blank partition: create the volume once. */
        MKFS_PARM opt = {.fmt = FM_ANY | FM_SFD, .n_fat = 1, .align = 0, .n_root = 0, .au_size = 4096};
        void *work = heap_caps_malloc(s_sector, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (work == NULL) return ESP_ERR_NO_MEM;
        result = f_mkfs(drive, &opt, work, (UINT)s_sector);
        heap_caps_free(work);
        if (result == FR_OK) result = f_mount(s_fs, drive, 1);
    }
    if (result != FR_OK) {
        ESP_LOGE(TAG, "TEST_FLASH_STORAGE: FAT mount failed (%d)", (int)result);
        return ESP_FAIL;
    }
    memset(&s_fake_card, 0, sizeof(s_fake_card));
    s_fake_card.csd.sector_size = (int)s_sector;
    s_fake_card.csd.capacity = (int)(wl_size(s_wl) / s_sector);
    sd_card_internal_set_card(&s_fake_card);
    sd_card_internal_set_recovery_unmount(sd_card_test_flash_unmount);
    ESP_LOGW(TAG, "TEST_FLASH_STORAGE mounted at %s (%u KiB), not an SD card; every block needs a %u byte DMA bounce buffer",
             sd_card_internal_mount_point(), (unsigned)(wl_size(s_wl) / 1024U), (unsigned)TEST_SD_DMA_BOUNCE_BYTES);
    return ESP_OK;
}
