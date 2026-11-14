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
