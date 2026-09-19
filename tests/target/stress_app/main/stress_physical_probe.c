#include "stress_physical_probe.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ad7606_driver.h"
#include "ad7606_serial.h"
#include "display.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_lcd_panel_ops.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "i2c_bus.h"
#include "nvs_flash.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "sensor_bus.h"
#include "spi_bus.h"


#define STRESS_PHYSICAL_AD7606_SAMPLES 32U
#define STRESS_PHYSICAL_SD_TESTS 8U
#define STRESS_PHYSICAL_TFT_DRAWS 32U
#define STRESS_PHYSICAL_TFT_BANDS 8U
#define STRESS_PHYSICAL_TFT_BAND_HEIGHT (DISPLAY_HEIGHT / STRESS_PHYSICAL_TFT_BANDS)
#define STRESS_PHYSICAL_I2C_READS 32U
#define STRESS_PHYSICAL_WIFI_SCANS 3U


static const char *result_name(bool pass)
{
    return pass ? "PASS" : "FAIL";
}


static void probe_ad7606(void)
{
    bool initialized = false;
    bool presence = false;
    uint32_t conversion_ok = 0U;
    uint32_t transport_ok = 0U;
    uint32_t varied_payloads = 0U;
    ad7606_serial_observation_t observation = {0};

    esp_err_t err = ad7606_driver_init();
    if (err == ESP_OK) {
        initialized = true;
        err = ad7606_driver_presence_monitor_start();
        presence = err == ESP_OK;
        if (presence) {
            for (uint32_t index = 0U; index < STRESS_PHYSICAL_AD7606_SAMPLES; ++index) {
                ad7606_sample_t sample = {0};
                if (ad7606_driver_read(&sample) != ESP_OK) {
                    continue;
                }
                conversion_ok++;
                if (ad7606_driver_get_last_serial_observation(&observation) == ESP_OK) {
                    transport_ok += observation.spi_transaction_ok ? 1U : 0U;
                    varied_payloads +=
                        observation.pattern == AD7606_SERIAL_PATTERN_VARIED ? 1U : 0U;
                }
            }
        }
    }

    const bool pass =
        initialized && presence &&
        conversion_ok == STRESS_PHYSICAL_AD7606_SAMPLES &&
        transport_ok == STRESS_PHYSICAL_AD7606_SAMPLES &&
        varied_payloads == STRESS_PHYSICAL_AD7606_SAMPLES;
    printf(
        "TARGET_STRESS_PHYSICAL name=ad7606 spi3=%s presence=%s samples=%u "
        "conversion_ok=%u transport_ok=%u varied_payloads=%u payload_pattern=%s verdict=%s\n",
        result_name(initialized && transport_ok == STRESS_PHYSICAL_AD7606_SAMPLES),
        result_name(presence),
        (unsigned)STRESS_PHYSICAL_AD7606_SAMPLES,
        (unsigned)conversion_ok,
        (unsigned)transport_ok,
        (unsigned)varied_payloads,
        ad7606_serial_pattern_name(observation.pattern),
        result_name(pass)
    );

    ad7606_driver_presence_monitor_stop();
    if (initialized) {
        ad7606_driver_deinit();
    }
}


static void probe_spi2_sd_and_tft(void)
{
    const esp_err_t spi_err = app_spi_bus_init();
    esp_err_t sd_mount_err = ESP_ERR_INVALID_STATE;
    uint32_t sd_passes = 0U;
    esp_err_t tft_init_err = ESP_ERR_INVALID_STATE;
    uint32_t tft_draw_passes = 0U;

    if (spi_err == ESP_OK) {
        sd_mount_err = sd_card_mount();
        if (sd_mount_err == ESP_OK) {
            for (uint32_t index = 0U; index < STRESS_PHYSICAL_SD_TESTS; ++index) {
                sd_passes += sd_card_diagnostics_run_write_read_test() == ESP_OK ? 1U : 0U;
            }
        }

        tft_init_err = display_init();
        if (tft_init_err == ESP_OK) {
            static const uint16_t colors[STRESS_PHYSICAL_TFT_BANDS] = {
                0xF800U,
                0x07E0U,
                0x001FU,
                0xFFE0U,
                0xF81FU,
                0x07FFU,
                0xFFFFU,
                0x0000U,
            };
            static uint16_t pixels[DISPLAY_WIDTH * STRESS_PHYSICAL_TFT_BAND_HEIGHT];
            for (size_t index = 0U; index < sizeof(pixels) / sizeof(pixels[0]); ++index) {
                pixels[index] = colors[0];
            }
            for (uint32_t index = 0U; index < STRESS_PHYSICAL_TFT_DRAWS; ++index) {
                const uint32_t band = index % STRESS_PHYSICAL_TFT_BANDS;
                for (size_t pixel = 0U; pixel < sizeof(pixels) / sizeof(pixels[0]); ++pixel) {
                    pixels[pixel] = colors[band];
                }
                const esp_err_t draw_err = esp_lcd_panel_draw_bitmap(
                    display_panel_handle(),
                    0,
                    (int)(band * STRESS_PHYSICAL_TFT_BAND_HEIGHT),
                    DISPLAY_WIDTH,
                    (int)((band + 1U) * STRESS_PHYSICAL_TFT_BAND_HEIGHT),
                    pixels
                );
                tft_draw_passes += draw_err == ESP_OK ? 1U : 0U;
            }
        }
    }

    const bool spi_pass = spi_err == ESP_OK;
    const bool sd_pass =
        sd_mount_err == ESP_OK && sd_passes == STRESS_PHYSICAL_SD_TESTS;
    const bool tft_pass =
        tft_init_err == ESP_OK && tft_draw_passes == STRESS_PHYSICAL_TFT_DRAWS;
    printf(
        "TARGET_STRESS_PHYSICAL name=spi2_sd spi2=%s iterations=%u sd_mount=%s "
        "sd_rw_passes=%u verdict=%s\n",
        result_name(spi_pass),
        (unsigned)STRESS_PHYSICAL_SD_TESTS,
        result_name(sd_mount_err == ESP_OK),
        (unsigned)sd_passes,
        result_name(spi_pass && sd_pass)
    );
    printf(
        "TARGET_STRESS_PHYSICAL name=tft spi2=%s iterations=%u panel=%s "
        "draw_passes=%u verdict=%s\n",
        result_name(spi_pass),
        (unsigned)STRESS_PHYSICAL_TFT_DRAWS,
        result_name(tft_init_err == ESP_OK),
        (unsigned)tft_draw_passes,
        result_name(spi_pass && tft_pass)
    );

    if (tft_init_err == ESP_OK) {
        display_deinit();
    }
    if (sd_mount_err == ESP_OK) {
        (void)sd_card_unmount();
    }
}


static void probe_i2c(void)
{
    sensor_bus_handles_t handles = {0};
    i2c_bus_device_handle_t tca = NULL;
    uint8_t tca_address = NULL_I2C_DEV_ADDR;
    uint8_t ina_config_high = 0U;
    uint32_t ina_passes = 0U;

    const esp_err_t lock_err = sensor_bus_lock_init();
    const esp_err_t handles_err = lock_err == ESP_OK
        ? sensor_bus_get_handles(&handles)
        : lock_err;
    if (handles_err == ESP_OK) {
        for (uint32_t index = 0U; index < STRESS_PHYSICAL_I2C_READS; ++index) {
            ina_passes +=
                i2c_bus_read_byte(handles.ina3221, 0x00U, &ina_config_high) == ESP_OK
                    ? 1U
                    : 0U;
        }
    }
    const esp_err_t tca_err = handles_err == ESP_OK
        ? sensor_bus_get_tca9548a(&tca, &tca_address)
        : handles_err;
    const bool pass =
        handles_err == ESP_OK &&
        ina_passes == STRESS_PHYSICAL_I2C_READS &&
        tca_err == ESP_OK;

    printf(
        "TARGET_STRESS_PHYSICAL name=i2c i2c_bus=%s iterations=%u ina3221_passes=%u "
        "tca9548a=%s address=0x%02X verdict=%s\n",
        result_name(handles_err == ESP_OK),
        (unsigned)STRESS_PHYSICAL_I2C_READS,
        (unsigned)ina_passes,
        result_name(tca_err == ESP_OK),
        (unsigned)tca_address,
        result_name(pass)
    );
    (void)sensor_bus_deinit();
}


static void probe_wifi(void)
{
    bool netif_ok = false;
    bool event_ok = false;
    bool init_ok = false;
    bool mode_ok = false;
    bool start_ok = false;
    bool scan_ok = false;
    uint32_t scan_passes = 0U;
    uint16_t access_points = 0U;
    esp_netif_t *sta_netif = NULL;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }

    if (err == ESP_OK) {
        err = esp_netif_init();
        netif_ok = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    }
    if (netif_ok) {
        err = esp_event_loop_create_default();
        event_ok = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    }
    if (event_ok) {
        sta_netif = esp_netif_create_default_wifi_sta();
        netif_ok = sta_netif != NULL;
        if (netif_ok) {
            wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
            err = esp_wifi_init(&config);
            init_ok = err == ESP_OK;
            if (init_ok) {
                mode_ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
                start_ok = mode_ok && esp_wifi_start() == ESP_OK;
                if (start_ok) {
                    wifi_scan_config_t scan_config = {0};
                    for (uint32_t index = 0U; index < STRESS_PHYSICAL_WIFI_SCANS; ++index) {
                        err = esp_wifi_scan_start(&scan_config, true);
                        if (err == ESP_OK && esp_wifi_scan_get_ap_num(&access_points) == ESP_OK) {
                            scan_passes++;
                        }
                    }
                    scan_ok = scan_passes == STRESS_PHYSICAL_WIFI_SCANS;
                }
            }
        }
    }

    const bool pass = netif_ok && event_ok && init_ok && mode_ok && start_ok && scan_ok;
    printf(
        "TARGET_STRESS_PHYSICAL name=wifi radio=%s iterations=%u scan_passes=%u ap_count=%u "
        "association=NOT_REQUESTED verdict=%s\n",
        result_name(init_ok && mode_ok && start_ok),
        (unsigned)STRESS_PHYSICAL_WIFI_SCANS,
        (unsigned)scan_passes,
        (unsigned)access_points,
        result_name(pass)
    );

    if (init_ok) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
    }
    if (sta_netif != NULL) {
        esp_netif_destroy(sta_netif);
    }
}


void stress_physical_probe_run(void)
{
    printf("TARGET_STRESS_PHYSICAL_BEGIN\n");
    probe_ad7606();
    probe_spi2_sd_and_tft();
    probe_i2c();
    probe_wifi();
    printf("TARGET_STRESS_PHYSICAL_END\n");
}
