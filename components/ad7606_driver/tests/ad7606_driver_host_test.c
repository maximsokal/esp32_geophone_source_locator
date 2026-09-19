#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ad7606_driver.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static int fake_busy_level;
static int fake_spi_error;
static int fake_gpio_error;
static int fake_spi_initialize_calls;
static int fake_spi_free_calls;
static int fake_transmit_calls;
static int fake_gpio_config_calls;
static int64_t fake_time_us;
static uint8_t fake_rx[AD7606_CHANNEL_COUNT * 2];
static spi_bus_config_t last_bus_config;
static spi_device_interface_config_t last_device_config;

#define FAKE_GPIO_LEVEL_SLOTS 64
static int fake_gpio_level[FAKE_GPIO_LEVEL_SLOTS];

esp_err_t gpio_config(const gpio_config_t *config)
{
    (void)config;
    fake_gpio_config_calls++;
    return fake_gpio_error ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (gpio_num >= 0 && gpio_num < FAKE_GPIO_LEVEL_SLOTS) {
        fake_gpio_level[gpio_num] = (int)level;
    }
    return fake_gpio_error ? ESP_ERR_INVALID_STATE : ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return fake_busy_level;
}

esp_err_t spi_bus_initialize(
    spi_host_device_t host_id,
    const spi_bus_config_t *bus_config,
    int dma_chan
)
{
    assert(host_id == SPI3_HOST);
    assert(dma_chan == SPI_DMA_CH_AUTO);
    last_bus_config = *bus_config;
    fake_spi_initialize_calls++;
    return fake_spi_error ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t spi_bus_add_device(
    spi_host_device_t host_id,
    const spi_device_interface_config_t *config,
    spi_device_handle_t *handle
)
{
    assert(host_id == SPI3_HOST);
    last_device_config = *config;
    *handle = (spi_device_handle_t)(uintptr_t)0x1;
    return fake_spi_error ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    assert(handle == (spi_device_handle_t)(uintptr_t)0x1);
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    assert(host_id == SPI3_HOST);
    fake_spi_free_calls++;
    return ESP_OK;
}

esp_err_t spi_device_transmit(
    spi_device_handle_t handle,
    spi_transaction_t *transaction
)
{
    assert(handle == (spi_device_handle_t)(uintptr_t)0x1);
    assert(transaction->length == AD7606_CHANNEL_COUNT * 2U * 8U);
    fake_transmit_calls++;
    if (fake_spi_error) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < sizeof(fake_rx); ++i) {
        ((uint8_t *)transaction->rx_buffer)[i] = fake_rx[i];
    }
    /* Model a transport that completes well after CONVST timestamp capture. */
    fake_time_us += 100;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return fake_time_us;
}

void esp_rom_delay_us(uint32_t us)
{
    fake_time_us += (int64_t)us;
}

static void reset_fake(void)
{
    fake_busy_level = 0;
    fake_spi_error = 0;
    fake_gpio_error = 0;
    fake_spi_initialize_calls = 0;
    fake_spi_free_calls = 0;
    fake_transmit_calls = 0;
    fake_gpio_config_calls = 0;
    memset(fake_gpio_level, -1, sizeof(fake_gpio_level));
    fake_time_us = 1000;
    for (size_t i = 0; i < AD7606_CHANNEL_COUNT; ++i) {
        const int16_t value = (int16_t)((int)i * 1000 - 3000);
        fake_rx[i * 2] = (uint8_t)((uint16_t)value >> 8);
        fake_rx[i * 2 + 1] = (uint8_t)value;
    }
}

static void test_successful_init_and_conversion(void)
{
    reset_fake();
    assert(ad7606_driver_read(NULL) == ESP_ERR_INVALID_STATE);
    assert(ad7606_driver_init() == ESP_OK);
    assert(fake_spi_initialize_calls == 1);
    assert(last_bus_config.mosi_io_num == -1);
    assert(last_bus_config.miso_io_num == GPIO_NUM_39);
    assert(last_bus_config.sclk_io_num == GPIO_NUM_40);
    assert(last_bus_config.max_transfer_sz == AD7606_CHANNEL_COUNT * 2);
    assert(last_device_config.mode == AD7606_SPI_MODE);
    assert(last_device_config.clock_speed_hz == AD7606_BUS_CLOCK_HZ);
    assert(last_device_config.spics_io_num == GPIO_NUM_41);
    assert(last_device_config.queue_size == 1);
    assert(fake_transmit_calls == 1);

    ad7606_sample_t sample = { 0 };
    assert(ad7606_driver_read(NULL) == ESP_ERR_INVALID_ARG);
    assert(ad7606_driver_read(&sample) == ESP_OK);
    assert(fake_transmit_calls == 2);
    assert(sample.timestamp_us == 1138);
    for (size_t channel = 0; channel < AD7606_CHANNEL_COUNT; ++channel) {
        assert(sample.raw[channel] == (int16_t)((int)channel * 1000 - 3000));
    }
    assert(ad7606_raw_to_uv(-32768) == -5000000.0f);
    assert(ad7606_raw_to_uv(32767) > 4999840.0f);

    ad7606_driver_deinit();
    assert(fake_spi_free_calls == 1);
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
}

static void test_spi_error_does_not_publish_sample(void)
{
    reset_fake();
    assert(ad7606_driver_init() == ESP_OK);
    ad7606_sample_t sample = { 0 };
    fake_spi_error = 1;
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
    ad7606_driver_deinit();
}

static void test_constant_payload_is_observable_but_not_transport_failure(void)
{
    reset_fake();
    memset(fake_rx, 0x00, sizeof(fake_rx));
    assert(ad7606_driver_init() == ESP_OK);

    ad7606_serial_observation_t observation = {0};
    assert(ad7606_driver_get_last_serial_observation(&observation) == ESP_OK);
    assert(observation.conversion_complete);
    assert(observation.spi_transaction_ok);
    assert(observation.pattern == AD7606_SERIAL_PATTERN_ALL_00);
    assert(observation.rx[0] == 0x00U && observation.rx[15] == 0x00U);
    ad7606_driver_deinit();

    reset_fake();
    memset(fake_rx, 0xFF, sizeof(fake_rx));
    assert(ad7606_driver_init() == ESP_OK);
    assert(ad7606_driver_get_last_serial_observation(&observation) == ESP_OK);
    assert(observation.pattern == AD7606_SERIAL_PATTERN_ALL_FF);
    ad7606_driver_deinit();
}

static void test_busy_timeout_fails_initialization_and_releases_bus(void)
{
    reset_fake();
    fake_busy_level = 1;
    assert(ad7606_driver_init() == ESP_ERR_TIMEOUT);
    assert(fake_spi_free_calls == 1);
    ad7606_sample_t sample = { 0 };
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
    ad7606_driver_deinit();
}

static void test_spi_initialization_error_is_returned(void)
{
    reset_fake();
    fake_spi_error = 1;
    assert(ad7606_driver_init() == ESP_ERR_INVALID_STATE);
    assert(fake_spi_free_calls == 0);
    ad7606_sample_t sample = { 0 };
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
    ad7606_driver_deinit();
}

/*
 * Regression: the AD7606 is physically powered from board power-on, but
 * before this function existed, RESET/CONVST/BUSY were left in whatever
 * state the ESP32 came up in until the user opened Measurements or issued
 * a Web start request. That gap could be minutes or never. This must set
 * the same safe levels ad7606_driver_init() uses internally, and must not
 * touch SPI3 (it has to be safe to call at boot even with no card/ADC
 * physically present, and safe to call again when init() runs later).
 */
static void test_safe_gpio_init_sets_idle_levels_without_touching_spi(void)
{
    reset_fake();

    assert(ad7606_driver_safe_gpio_init() == ESP_OK);

    assert(fake_gpio_config_calls == 2);
    assert(fake_spi_initialize_calls == 0);
    assert(fake_spi_free_calls == 0);
    assert(fake_transmit_calls == 0);

    assert(fake_gpio_level[BOARD_AD7606_CS_GPIO] == 1);
    assert(fake_gpio_level[BOARD_AD7606_RESET_GPIO] == 0);
    assert(fake_gpio_level[BOARD_AD7606_CONVST_GPIO] == 0);

    /* Calling it again (as init() does internally) must stay side-effect free. */
    assert(ad7606_driver_safe_gpio_init() == ESP_OK);
    assert(fake_spi_initialize_calls == 0);

    /* And a real init() afterwards must still work normally. */
    assert(ad7606_driver_init() == ESP_OK);
    assert(fake_spi_initialize_calls == 1);
    ad7606_driver_deinit();
}


static void test_safe_gpio_init_propagates_gpio_failure(void)
{
    reset_fake();
    fake_gpio_error = 1;

    assert(ad7606_driver_safe_gpio_init() == ESP_ERR_INVALID_STATE);
    assert(fake_spi_initialize_calls == 0);
}


int main(void)
{
    test_successful_init_and_conversion();
    test_spi_error_does_not_publish_sample();
    test_constant_payload_is_observable_but_not_transport_failure();
    test_busy_timeout_fails_initialization_and_releases_bus();
    test_spi_initialization_error_is_returned();
    test_safe_gpio_init_sets_idle_levels_without_touching_spi();
    test_safe_gpio_init_propagates_gpio_failure();
    puts("AD7606 driver host tests passed");
    return 0;
}
