#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sd_card.h"
#include "sd_card_internal.h"


static esp_err_t s_gpio_config_result;
static esp_err_t s_gpio_set_result;
static esp_err_t s_mount_result;
static gpio_config_t s_gpio_config;
static gpio_num_t s_set_gpio;
static int s_set_level;
static sdmmc_host_t s_host;
static sdspi_device_config_t s_slot;
static esp_vfs_fat_mount_config_t s_mount_config;
static char s_mount_point[32];
static sdmmc_card_t s_card;
static unsigned s_gpio_config_calls;
static unsigned s_gpio_set_calls;
static unsigned s_mount_calls;
static unsigned s_print_calls;
static unsigned s_timeout_hint_logs;
static unsigned s_low_frequency_logs;


static void reset_fakes(void)
{
    s_gpio_config_result = ESP_OK;
    s_gpio_set_result = ESP_OK;
    s_mount_result = ESP_OK;
    memset(&s_gpio_config, 0, sizeof(s_gpio_config));
    s_set_gpio = -1;
    s_set_level = -1;
    memset(&s_host, 0, sizeof(s_host));
    memset(&s_slot, 0, sizeof(s_slot));
    memset(&s_mount_config, 0, sizeof(s_mount_config));
    memset(s_mount_point, 0, sizeof(s_mount_point));
    s_card.max_freq_khz = SDMMC_FREQ_DEFAULT;
    s_card.real_freq_khz = SDMMC_FREQ_DEFAULT;
    s_gpio_config_calls = 0U;
    s_gpio_set_calls = 0U;
    s_mount_calls = 0U;
    s_print_calls = 0U;
    s_timeout_hint_logs = 0U;
    s_low_frequency_logs = 0U;
    sd_card_internal_set_card(NULL);
}


const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_ERR_TIMEOUT:
            return "ESP_ERR_TIMEOUT";
        default:
            return "ESP_FAIL";
    }
}


void fake_esp_log(const char *tag, const char *format, ...)
{
    (void)tag;
    if (strstr(format, "Card did not answer") != NULL) {
        s_timeout_hint_logs++;
    }
    if (strstr(format, "below the expected 20 MHz class") != NULL) {
        s_low_frequency_logs++;
    }

    va_list args;
    va_start(args, format);
    va_end(args);
}


esp_err_t gpio_config(const gpio_config_t *config)
{
    s_gpio_config_calls++;
    if (config != NULL) {
        s_gpio_config = *config;
    }
    return s_gpio_config_result;
}


esp_err_t gpio_set_level(gpio_num_t gpio_num, int level)
{
    s_gpio_set_calls++;
    s_set_gpio = gpio_num;
    s_set_level = level;
    return s_gpio_set_result;
}


int gpio_get_level(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return 1;
}


esp_err_t esp_vfs_fat_sdspi_mount(
    const char *base_path,
    const sdmmc_host_t *host_config,
    const sdspi_device_config_t *slot_config,
    const esp_vfs_fat_mount_config_t *mount_config,
    sdmmc_card_t **out_card
)
{
    s_mount_calls++;
    assert(base_path != NULL);
    assert(host_config != NULL);
    assert(slot_config != NULL);
    assert(mount_config != NULL);
    assert(out_card != NULL);

    snprintf(s_mount_point, sizeof(s_mount_point), "%s", base_path);
    s_host = *host_config;
    s_slot = *slot_config;
    s_mount_config = *mount_config;

    if (s_mount_result == ESP_OK) {
        *out_card = &s_card;
    }
    return s_mount_result;
}


void sdmmc_card_print_info(FILE *stream, const sdmmc_card_t *card)
{
    assert(stream == stdout);
    assert(card == &s_card);
    s_print_calls++;
}


static void test_success_and_repeated_mount_are_idempotent(void)
{
    reset_fakes();

    assert(sd_card_mount() == ESP_OK);
    assert(sd_card_internal_card() == &s_card);
    assert(s_gpio_config_calls == 1U);
    assert(s_gpio_set_calls == 1U);
    assert(s_mount_calls == 1U);
    assert(s_print_calls == 1U);

    assert(s_gpio_config.pin_bit_mask == (1ULL << BOARD_TFT_CS_GPIO));
    assert(s_gpio_config.mode == GPIO_MODE_OUTPUT);
    assert(s_gpio_config.pull_up_en == GPIO_PULLUP_DISABLE);
    assert(s_gpio_config.pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(s_gpio_config.intr_type == GPIO_INTR_DISABLE);
    assert(s_set_gpio == BOARD_TFT_CS_GPIO);
    assert(s_set_level == 1);

    assert(strcmp(s_mount_point, "/sdcard") == 0);
    assert(s_host.slot == BOARD_SPI_HOST);
    assert(s_host.max_freq_khz == SDMMC_FREQ_DEFAULT);
    assert(s_slot.host_id == BOARD_SPI_HOST);
    assert(s_slot.gpio_cs == BOARD_SD_CS_GPIO);
    assert(!s_mount_config.format_if_mount_failed);
    assert(s_mount_config.max_files == 5);
    assert(s_mount_config.allocation_unit_size == 32U * 1024U);
    assert(!s_mount_config.disk_status_check_enable);

    assert(sd_card_mount() == ESP_OK);
    assert(s_gpio_config_calls == 1U);
    assert(s_gpio_set_calls == 1U);
    assert(s_mount_calls == 1U);
    assert(s_print_calls == 1U);
}


static void test_gpio_config_failure_stops_before_mount(void)
{
    reset_fakes();
    s_gpio_config_result = ESP_FAIL;

    assert(sd_card_mount() == ESP_FAIL);
    assert(sd_card_internal_card() == NULL);
    assert(s_gpio_config_calls == 1U);
    assert(s_gpio_set_calls == 0U);
    assert(s_mount_calls == 0U);
}


static void test_gpio_set_failure_stops_before_mount(void)
{
    reset_fakes();
    s_gpio_set_result = ESP_FAIL;

    assert(sd_card_mount() == ESP_FAIL);
    assert(sd_card_internal_card() == NULL);
    assert(s_gpio_config_calls == 1U);
    assert(s_gpio_set_calls == 1U);
    assert(s_mount_calls == 0U);
}


static void test_timeout_does_not_publish_card_and_emits_hint(void)
{
    reset_fakes();
    s_mount_result = ESP_ERR_TIMEOUT;

    assert(sd_card_mount() == ESP_ERR_TIMEOUT);
    assert(sd_card_internal_card() == NULL);
    assert(s_mount_calls == 1U);
    assert(s_print_calls == 0U);
    assert(s_timeout_hint_logs == 1U);
}


static void test_generic_mount_failure_does_not_publish_card(void)
{
    reset_fakes();
    s_mount_result = ESP_FAIL;

    assert(sd_card_mount() == ESP_FAIL);
    assert(sd_card_internal_card() == NULL);
    assert(s_mount_calls == 1U);
    assert(s_print_calls == 0U);
    assert(s_timeout_hint_logs == 0U);
}


static void test_low_negotiated_frequency_is_visible_but_not_mount_failure(void)
{
    reset_fakes();
    s_card.max_freq_khz = SDMMC_FREQ_DEFAULT;
    s_card.real_freq_khz = 12000;

    assert(sd_card_mount() == ESP_OK);
    assert(sd_card_internal_card() == &s_card);
    assert(s_low_frequency_logs == 1U);
    assert(s_print_calls == 1U);
}


int main(void)
{
    test_success_and_repeated_mount_are_idempotent();
    test_gpio_config_failure_stops_before_mount();
    test_gpio_set_failure_stops_before_mount();
    test_timeout_does_not_publish_card_and_emits_hint();
    test_generic_mount_failure_does_not_publish_card();
    test_low_negotiated_frequency_is_visible_but_not_mount_failure();
    return 0;
}
