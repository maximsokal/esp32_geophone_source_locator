#include "esp_err.h"
#include "esp_log.h"

#include "spi_bus.h"
#include "sd_card.h"
#include "display.h"

static const char *TAG = "APP";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Geophone Source Locator");

    ESP_ERROR_CHECK(app_spi_bus_init());

    /*
     * ВАЖНО:
     * SD и TFT находятся на общей SPI2-шине.
     * SD сначала переводим в SPI mode,
     * и только затем начинаем работать с TFT.
     */
    ESP_ERROR_CHECK(sd_card_mount());
    ESP_ERROR_CHECK(sd_card_test());

    ESP_ERROR_CHECK(display_init());

    ESP_ERROR_CHECK(display_fill(0x07E0));

    ESP_LOGI(TAG, "Display: OK");
    ESP_LOGI(TAG, "SD card: OK");
    ESP_LOGI(TAG, "Hardware bring-up completed");
}