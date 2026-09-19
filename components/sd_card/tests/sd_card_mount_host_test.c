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
static unsigned s_gpio_set_fail_on_call;
static esp_err_t s_mount_result;
static bool s_mount_without_card;
static esp_err_t s_unmount_result;
static esp_err_t s_info_result;
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
static unsigned s_unmount_calls;
static unsigned s_info_calls;
static unsigned s_print_calls;
static sdmmc_card_t *s_last_unmount_card;
static unsigned s_timeout_hint_logs;
static unsigned s_low_frequency_logs;

#define FAKE_GPIO_COUNT 64
#define FAKE_GPIO_SET_HISTORY_CAPACITY 8
static bool s_gpio_input_enabled[FAKE_GPIO_COUNT];
static int s_gpio_level[FAKE_GPIO_COUNT];
static gpio_num_t s_set_gpio_history[FAKE_GPIO_SET_HISTORY_CAPACITY];
static int s_set_level_history[FAKE_GPIO_SET_HISTORY_CAPACITY];
static char s_last_levels_log[192];


static void reset_fakes(void)
{
    s_gpio_config_result = ESP_OK;
    s_gpio_set_result = ESP_OK;
    s_gpio_set_fail_on_call = 0U;
    s_mount_result = ESP_OK;
    s_mount_without_card = false;
    s_unmount_result = ESP_OK;
    s_info_result = ESP_OK;
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
    s_unmount_calls = 0U;
    s_info_calls = 0U;
    s_print_calls = 0U;
    s_last_unmount_card = NULL;
    s_timeout_hint_logs = 0U;
    s_low_frequency_logs = 0U;
    memset(s_gpio_input_enabled, 0, sizeof(s_gpio_input_enabled));
    memset(s_gpio_level, 0, sizeof(s_gpio_level));
    for (size_t i = 0; i < FAKE_GPIO_SET_HISTORY_CAPACITY; ++i) {
        s_set_gpio_history[i] = -1;
        s_set_level_history[i] = -1;
    }
    memset(s_last_levels_log, 0, sizeof(s_last_levels_log));
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
    if (strstr(format, "SPI2 levels") != NULL) {
        vsnprintf(s_last_levels_log, sizeof(s_last_levels_log), format, args);
    }
    va_end(args);
}


esp_err_t gpio_config(const gpio_config_t *config)
{
    s_gpio_config_calls++;
    if (config != NULL) {
        s_gpio_config = *config;

        if (s_gpio_config_result == ESP_OK) {
            for (int pin = 0; pin < FAKE_GPIO_COUNT; ++pin) {
                if ((config->pin_bit_mask & (1ULL << pin)) != 0ULL) {
                    s_gpio_input_enabled[pin] =
                        (config->mode & GPIO_MODE_DEF_INPUT) != 0;
                }
            }
        }
    }
    return s_gpio_config_result;
}


esp_err_t gpio_set_level(gpio_num_t gpio_num, int level)
{
    s_gpio_set_calls++;
    s_set_gpio = gpio_num;
    s_set_level = level;
    if (s_gpio_set_calls <= FAKE_GPIO_SET_HISTORY_CAPACITY) {
        const size_t index = s_gpio_set_calls - 1U;
        s_set_gpio_history[index] = gpio_num;
        s_set_level_history[index] = level;
    }
    const bool fail_this_call =
        s_gpio_set_fail_on_call != 0U &&
        s_gpio_set_calls == s_gpio_set_fail_on_call;
    if (s_gpio_set_result == ESP_OK &&
        !fail_this_call &&
        gpio_num >= 0 && gpio_num < FAKE_GPIO_COUNT) {
        s_gpio_level[gpio_num] = level;
    }
    return fail_this_call ? ESP_FAIL : s_gpio_set_result;
}


/*
 * Models ESP-IDF: gpio_config() disables the input path for modes without
 * GPIO_MODE_DEF_INPUT, so such a pin always reads back 0 no matter what level
 * is driven. The previous fake returned a constant 1, which is why no host
 * test could catch a diagnostic reading an output-only pin.
 */
int gpio_get_level(gpio_num_t gpio_num)
{
    if (gpio_num < 0 || gpio_num >= FAKE_GPIO_COUNT) {
        return 0;
    }
    return s_gpio_input_enabled[gpio_num] ? s_gpio_level[gpio_num] : 0;
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

    if (s_mount_result == ESP_OK && !s_mount_without_card) {
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


esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card)
{
    assert(strcmp(base_path, "/sdcard") == 0);
    ++s_unmount_calls;
    s_last_unmount_card = card;
    return s_unmount_result;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total_bytes, uint64_t *free_bytes)
{
    assert(strcmp(base_path, "/sdcard") == 0);
    assert(total_bytes != NULL);
    assert(free_bytes != NULL);
    ++s_info_calls;
    *total_bytes = 1024U;
    *free_bytes = 512U;
    return s_info_result;
}


static void test_success_and_repeated_mount_are_idempotent(void)
{
    reset_fakes();

    assert(sd_card_mount() == ESP_OK);
    assert(sd_card_internal_card() == &s_card);
    assert(s_gpio_config_calls == 1U);
    assert(s_gpio_set_calls == 2U);
    assert(s_mount_calls == 1U);
    assert(s_print_calls == 1U);

    assert(s_gpio_config.pin_bit_mask ==
           ((1ULL << BOARD_TFT_CS_GPIO) | (1ULL << BOARD_SD_CS_GPIO)));
    assert(s_gpio_config.mode == GPIO_MODE_INPUT_OUTPUT);
    assert((s_gpio_config.mode & GPIO_MODE_DEF_INPUT) != 0);
    assert(s_gpio_config.pull_up_en == GPIO_PULLUP_DISABLE);
    assert(s_gpio_config.pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(s_gpio_config.intr_type == GPIO_INTR_DISABLE);
    assert(gpio_get_level(BOARD_TFT_CS_GPIO) == 1);
    assert(gpio_get_level(BOARD_SD_CS_GPIO) == 1);

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
    assert(s_gpio_set_calls == 2U);
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


static void test_sd_cs_set_failure_stops_before_mount(void)
{
    reset_fakes();
    s_gpio_set_fail_on_call = 2U;

    assert(sd_card_mount() == ESP_FAIL);
    assert(sd_card_internal_card() == NULL);
    assert(s_gpio_config_calls == 1U);
    assert(s_gpio_set_calls == 2U);
    assert(s_mount_calls == 0U);
}


static void test_success_without_card_handle_is_rejected(void)
{
    reset_fakes();
    s_mount_without_card = true;

    assert(sd_card_mount() == ESP_ERR_INVALID_STATE);
    assert(sd_card_internal_card() == NULL);
    assert(s_mount_calls == 1U);
    assert(s_print_calls == 0U);
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


static void test_unmount_success_consumes_handle(void)
{
    reset_fakes();
    assert(sd_card_mount() == ESP_OK);
    assert(sd_card_is_mounted());
    assert(s_info_calls == 1U);

    assert(sd_card_unmount() == ESP_OK);
    assert(s_unmount_calls == 1U);
    assert(s_last_unmount_card == &s_card);
    assert(sd_card_internal_card() == NULL);
    assert(!sd_card_is_mounted());
    assert(s_info_calls == 1U);

    assert(sd_card_unmount() == ESP_OK);
    assert(s_unmount_calls == 1U);
}

static void test_unmount_cleanup_error_still_consumes_handle(void)
{
    reset_fakes();
    assert(sd_card_mount() == ESP_OK);
    s_unmount_result = ESP_ERR_INVALID_STATE;

    assert(sd_card_unmount() == ESP_ERR_INVALID_STATE);
    assert(s_unmount_calls == 1U);
    assert(s_last_unmount_card == &s_card);
    assert(sd_card_internal_card() == NULL);
    assert(!sd_card_is_mounted());
    assert(s_info_calls == 0U);

    assert(sd_card_unmount() == ESP_OK);
    assert(s_unmount_calls == 1U);
}

static void test_mount_after_consuming_unmount_attempt_uses_fresh_mount_path(void)
{
    reset_fakes();
    assert(sd_card_mount() == ESP_OK);
    s_unmount_result = ESP_ERR_INVALID_STATE;
    assert(sd_card_unmount() == ESP_ERR_INVALID_STATE);
    assert(sd_card_internal_card() == NULL);

    s_unmount_result = ESP_OK;
    assert(sd_card_mount() == ESP_OK);
    assert(s_mount_calls == 2U);
    assert(sd_card_internal_card() == &s_card);
}


/*
 * Regression: the mount-failure diagnostic reported a permanent "TFT_CS=0"
 * on real hardware because the pin was configured output-only and therefore
 * unreadable. That false reading looked like SPI bus contention and sent
 * several rounds of hardware debugging down the wrong path.
 */
static void test_mount_failure_diagnostic_reports_true_cs_level(void)
{
    reset_fakes();
    s_mount_result = ESP_ERR_TIMEOUT;

    assert(sd_card_mount() == ESP_ERR_TIMEOUT);

    assert(s_set_gpio_history[0] == BOARD_TFT_CS_GPIO);
    assert(s_set_level_history[0] == 1);
    assert(s_set_gpio_history[1] == BOARD_SD_CS_GPIO);
    assert(s_set_level_history[1] == 1);
    assert(gpio_get_level(BOARD_TFT_CS_GPIO) == 1);
    assert(gpio_get_level(BOARD_SD_CS_GPIO) == 1);
    assert(strstr(s_last_levels_log, "TFT_CS=1") != NULL);
    assert(strstr(s_last_levels_log, "SD_CS=1") != NULL);
    assert(strstr(s_last_levels_log, "TFT_CS=0") == NULL);
}


int main(void)
{
    test_mount_failure_diagnostic_reports_true_cs_level();
    test_success_and_repeated_mount_are_idempotent();
    test_gpio_config_failure_stops_before_mount();
    test_gpio_set_failure_stops_before_mount();
    test_sd_cs_set_failure_stops_before_mount();
    test_timeout_does_not_publish_card_and_emits_hint();
    test_generic_mount_failure_does_not_publish_card();
    test_success_without_card_handle_is_rejected();
    test_low_negotiated_frequency_is_visible_but_not_mount_failure();
    test_unmount_success_consumes_handle();
    test_unmount_cleanup_error_still_consumes_handle();
    test_mount_after_consuming_unmount_attempt_uses_fresh_mount_path();
    return 0;
}
