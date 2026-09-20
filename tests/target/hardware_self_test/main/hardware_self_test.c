#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "HW_SELF_TEST";


static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default: return "UNRECOGNIZED";
    }
}


static void log_result(const char *name, bool pass, const char *detail)
{
    ESP_LOGI(
        TAG,
        "[%s] %s%s%s",
        pass ? "PASS" : "FAIL",
        name,
        detail == NULL ? "" : " - ",
        detail == NULL ? "" : detail
    );
}


static void run_chip_test(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint8_t mac[6] = { 0 };
    const esp_err_t mac_err =
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char detail[128];
    snprintf(
        detail,
        sizeof(detail),
        "model=%d revision=%d cores=%d wifi=%s ble=%s mac=%02X:%02X:%02X:%02X:%02X:%02X",
        info.model,
        info.revision,
        info.cores,
        (info.features & CHIP_FEATURE_WIFI_BGN) != 0 ? "yes" : "no",
        (info.features & CHIP_FEATURE_BLE) != 0 ? "yes" : "no",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );

    log_result(
        "CHIP_CPU_ROM_MAC",
        info.model == CHIP_ESP32S3 &&
        info.cores == 2 &&
        mac_err == ESP_OK,
        detail
    );
}


static void run_flash_test(void)
{
    uint32_t flash_id = 0;
    uint32_t flash_size = 0;

    const esp_err_t id_err =
        esp_flash_read_id(NULL, &flash_id);
    const esp_err_t size_err =
        esp_flash_get_physical_size(NULL, &flash_size);

    char detail[128];
    snprintf(
        detail,
        sizeof(detail),
        "jedec=0x%06" PRIx32 " physical=%" PRIu32 " bytes",
        flash_id,
        flash_size
    );

    log_result(
        "FLASH_RDID_SIZE",
        id_err == ESP_OK &&
        size_err == ESP_OK &&
        flash_size >= (16U * 1024U * 1024U),
        detail
    );
}


static void run_memory_tests(void)
{
    const bool psram_ok =
        esp_psram_is_initialized() &&
        esp_psram_get_size() >= (8U * 1024U * 1024U);

    char psram_detail[96];
    snprintf(
        psram_detail,
        sizeof(psram_detail),
        "initialized=%s size=%zu bytes",
        esp_psram_is_initialized() ? "yes" : "no",
        esp_psram_get_size()
    );
    log_result("PSRAM_RUNTIME", psram_ok, psram_detail);

    const bool heap_ok =
        heap_caps_check_integrity_all(true);
    char heap_detail[96];
    snprintf(
        heap_detail,
        sizeof(heap_detail),
        "free=%" PRIu32 " largest=%zu",
        esp_get_free_heap_size(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
    );
    log_result("HEAP_INTEGRITY", heap_ok, heap_detail);
}


static esp_err_t wifi_check(
    const char *name,
    esp_err_t err
)
{
    ESP_LOGI(
        TAG,
        "WIFI_API %s: %s (0x%x)",
        name,
        esp_err_to_name(err),
        (unsigned)err
    );
    return err;
}


static bool run_wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(
            TAG,
            "NVS requires erase; refusing destructive erase in self-test"
        );
        log_result("NVS_INIT", false, esp_err_to_name(err));
        return false;
    }
    if (wifi_check("nvs_flash_init", err) != ESP_OK) {
        log_result("NVS_INIT", false, esp_err_to_name(err));
        return false;
    }
    log_result("NVS_INIT", true, "no erase performed");

    err = esp_netif_init();
    if (wifi_check("esp_netif_init", err) != ESP_OK) {
        return false;
    }

    err = esp_event_loop_create_default();
    if (wifi_check("esp_event_loop_create_default", err) != ESP_OK) {
        return false;
    }

    if (esp_netif_create_default_wifi_sta() == NULL ||
        esp_netif_create_default_wifi_ap() == NULL) {
        log_result("NETIF_CREATE", false, "default STA/AP netif");
        return false;
    }
    log_result("NETIF_CREATE", true, "default STA/AP netif");

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (wifi_check("esp_wifi_init", err) != ESP_OK) {
        return false;
    }
    log_result("WIFI_DRIVER_INIT", true, "driver returned ESP_OK");

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (wifi_check("esp_wifi_set_storage(RAM)", err) != ESP_OK) {
        return false;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (wifi_check("esp_wifi_set_mode(STA)", err) != ESP_OK) {
        return false;
    }

    ESP_LOGI(TAG, "WIFI_PHY_START_BEGIN");
    err = esp_wifi_start();
    ESP_LOGI(
        TAG,
        "WIFI_API esp_wifi_start: %s (0x%x)",
        esp_err_to_name(err),
        (unsigned)err
    );
    if (err != ESP_OK) {
        log_result("WIFI_PHY_START", false, esp_err_to_name(err));
        return false;
    }
    log_result("WIFI_PHY_START", true, "esp_wifi_start returned ESP_OK");
    return true;
}


static bool run_wifi_scan(void)
{
    ESP_LOGI(TAG, "WIFI_SCAN_BEGIN");
    esp_err_t err =
        esp_wifi_scan_start(NULL, true);
    if (wifi_check("esp_wifi_scan_start", err) != ESP_OK) {
        log_result("WIFI_SCAN_API", false, esp_err_to_name(err));
        return false;
    }

    uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (wifi_check("esp_wifi_scan_get_ap_num", err) != ESP_OK) {
        log_result("WIFI_SCAN_API", false, esp_err_to_name(err));
        return false;
    }

    wifi_ap_record_t *records = NULL;
    if (count > 0) {
        records = calloc(count, sizeof(*records));
        if (records == NULL) {
            log_result("WIFI_SCAN_API", false, "record allocation failed");
            return false;
        }
        uint16_t returned = count;
        err = esp_wifi_scan_get_ap_records(&returned, records);
        if (wifi_check("esp_wifi_scan_get_ap_records", err) != ESP_OK) {
            free(records);
            log_result("WIFI_SCAN_API", false, esp_err_to_name(err));
            return false;
        }
        for (uint16_t i = 0; i < returned; ++i) {
            ESP_LOGI(
                TAG,
                "WIFI_AP[%u] ssid=\"%s\" rssi=%d channel=%u auth=%d",
                (unsigned)i,
                (const char *)records[i].ssid,
                records[i].rssi,
                (unsigned)records[i].primary,
                records[i].authmode
            );
        }
        free(records);
    }

    char detail[64];
    snprintf(detail, sizeof(detail), "api=pass ap_count=%u", (unsigned)count);
    log_result("WIFI_SCAN_API", true, detail);
    if (count == 0) {
        ESP_LOGW(TAG, "WIFI_SCAN_NO_AP: RX result is inconclusive in this environment");
    }
    return true;
}


static bool run_softap(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        log_result("WIFI_SOFTAP", false, "MAC read failed");
        return false;
    }

    char ssid[33];
    snprintf(
        ssid,
        sizeof(ssid),
        "ESP32_DIAG_%02X%02X",
        mac[4],
        mac[5]
    );

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (wifi_check("esp_wifi_set_mode(AP)", err) != ESP_OK) {
        log_result("WIFI_SOFTAP", false, esp_err_to_name(err));
        return false;
    }

    wifi_config_t ap_config = { 0 };
    memcpy(ap_config.ap.ssid, ssid, strlen(ssid));
    ap_config.ap.ssid_len = strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (wifi_check("esp_wifi_set_config(AP)", err) != ESP_OK) {
        log_result("WIFI_SOFTAP", false, esp_err_to_name(err));
        return false;
    }

    char detail[96];
    snprintf(
        detail,
        sizeof(detail),
        "ssid=%s channel=%u open",
        ssid,
        (unsigned)ap_config.ap.channel
    );
    log_result("WIFI_SOFTAP", true, detail);
    ESP_LOGI(TAG, "WIFI_SOFTAP_READY ssid=\"%s\" password=\"\"", ssid);
    ESP_LOGI(TAG, "WIFI_SOFTAP_KEEPALIVE seconds=60");

    for (unsigned second = 0; second < 60; ++second) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (second == 0 || (second + 1) % 10 == 0) {
            ESP_LOGI(
                TAG,
                "WIFI_SOFTAP_ALIVE elapsed_s=%u",
                second + 1
            );
        }
    }
    return true;
}


void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 HARDWARE SELF-TEST BEGIN ===");
    ESP_LOGI(TAG, "reset_reason=%s", reset_reason_name(esp_reset_reason()));
    ESP_LOGI(TAG, "idf=%s", esp_get_idf_version());

    run_chip_test();
    run_flash_test();
    run_memory_tests();

    if (!run_wifi_init()) {
        ESP_LOGE(TAG, "BOARD RESULT: FAIL at Wi-Fi PHY/driver start");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    const bool scan_ok = run_wifi_scan();
    const bool softap_ok = run_softap();
    ESP_LOGI(
        TAG,
        "BOARD RESULT: %s scan_api=%s softap_api=%s",
        scan_ok && softap_ok ? "PASS" : "PARTIAL",
        scan_ok ? "PASS" : "FAIL",
        softap_ok ? "PASS" : "FAIL"
    );

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
