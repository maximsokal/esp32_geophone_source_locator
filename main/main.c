#include "esp_err.h"
#include "esp_log.h"

#include "spi_bus.h"
#include "sd_card.h"
#include "display.h"
#include "power_control.h"

static const char *TAG = "APP";


void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Geophone Source Locator");

    /*
     * В первую очередь настраиваем управление питанием.
     */
    ESP_ERROR_CHECK(power_control_init());

    /*
     * Общая SPI2-шина TFT + microSD.
     */
    ESP_ERROR_CHECK(app_spi_bus_init());

    /*
     * Сначала SD.
     */
    ESP_ERROR_CHECK(sd_card_mount());
    ESP_ERROR_CHECK(sd_card_test());

    /*
     * Затем TFT.
     */
    ESP_ERROR_CHECK(display_init());

    // Зеленый экран = устройство готово
    ESP_ERROR_CHECK(display_fill(0x07E0));

    ESP_LOGI(TAG, "Display: OK");
    ESP_LOGI(TAG, "SD card: OK");
    ESP_LOGI(TAG, "Hardware bring-up completed");

    /*
     * Здесь приложение работает и ждёт следующего
     * нажатия кнопки.
     */
    power_control_wait_for_shutdown_request();

    /*
     * Начинаем безопасное выключение.
     */
    ESP_LOGW(TAG, "Safe shutdown started");

    /*
     * Сейчас sd_card_test() уже закрыл test.txt.
     * Размонтируем файловую систему.
     */
    sd_card_unmount();

    ESP_LOGI(TAG, "SD card unmounted");

    /*
     * Гасим дисплей.
     * Ошибка дисплея не должна помешать отключению питания.
     */
    esp_err_t display_err = display_fill(0x0000);

    if (display_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to clear display: %s",
            esp_err_to_name(display_err)
        );
    }

    ESP_LOGI(TAG, "Display off");
    ESP_LOGI(TAG, "Cutting power");

    /*
     * GPIO18 -> OFF Pololu.
     */
    power_control_shutdown();
}