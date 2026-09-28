#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ad7606_driver.h"
#include "board_config.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static int fake_busy_level;
static unsigned fake_convst_pulses;
static unsigned fake_measured_busy_reads;
static uint32_t fake_cycle_count;
static int fake_spi_error;
static int fake_gpio_error;
static int fake_spi_initialize_calls;
static int fake_spi_free_calls;
static int fake_transmit_calls;
static int fake_freq_limit_calls;
static bool fake_freq_limit_gpio_used;
static int fake_freq_limit_input_delay_ns;
static int64_t fake_time_us;
static uint8_t fake_rx[AD7606_CHANNEL_COUNT * 2];
static spi_transaction_t *last_transaction;
static void *last_rx_buffer;
static spi_bus_config_t last_bus_config;
static spi_device_interface_config_t last_device_config;

/*
 * MAX-242: a per-pin GPIO model (mode + output register), so pin polarity
 * and Hi-Z release are actually asserted. `cs_driven_low_events` counts
 * every moment the CS pin is an OUTPUT with a LOW output register -- the
 * firmware must never do that (SPI3 owns CS only inside transactions, and
 * this model does not route SPI3 through the GPIO calls).
 */
#define FAKE_GPIO_COUNT 64
static gpio_mode_t fake_pin_mode[FAKE_GPIO_COUNT];
static int fake_pin_level[FAKE_GPIO_COUNT];
static int fake_pin_configured[FAKE_GPIO_COUNT];
static int cs_driven_low_events;

static void check_cs(void)
{
    if (fake_pin_mode[GPIO_NUM_41] == GPIO_MODE_OUTPUT && fake_pin_level[GPIO_NUM_41] == 0) {
        cs_driven_low_events++;
    }
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    if (fake_gpio_error) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int pin = 0; pin < FAKE_GPIO_COUNT; ++pin) {
        if ((config->pin_bit_mask & (1ULL << pin)) != 0U) {
            fake_pin_mode[pin] = config->mode;
            fake_pin_configured[pin] = 1;
            /* Every pin this driver configures must have its pulls off. */
            assert(config->pull_up_en == GPIO_PULLUP_DISABLE);
            assert(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
        }
    }
    check_cs();
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (fake_gpio_error) {
        return ESP_ERR_INVALID_STATE;
    }
    assert(gpio_num >= 0 && gpio_num < FAKE_GPIO_COUNT);
    if (gpio_num == BOARD_AD7606_CONVST_GPIO && fake_pin_level[gpio_num] != 0 && level == 0U) {
        ++fake_convst_pulses;
    }
    fake_pin_level[gpio_num] = level != 0U ? 1 : 0;
    check_cs();
    return ESP_OK;
}

static void assert_ad7606_pins_hi_z(void)
{
    const int pins[4] = { GPIO_NUM_2, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42 };
    for (size_t i = 0; i < 4U; ++i) {
        assert(fake_pin_configured[pins[i]]);
        assert(fake_pin_mode[pins[i]] == GPIO_MODE_INPUT);
    }
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if (gpio_num == BOARD_AD7606_BUSY_GPIO && fake_convst_pulses >= 2U) {
        return fake_measured_busy_reads++ == 0U ? 1 : 0;
    }
    return fake_busy_level;
}

uint32_t esp_cpu_get_cycle_count(void)
{
    fake_cycle_count += 960U; /* At 240 MHz, the scripted BUSY pulse measures 4 us. */
    return fake_cycle_count;
}

int esp_clk_cpu_freq(void)
{
    return 240000000;
}

esp_err_t spi_bus_initialize(
    spi_host_device_t host_id,
    const spi_bus_config_t *bus_config,
    int dma_chan
)
{
    assert(host_id == SPI3_HOST);
    assert(dma_chan == SPI_DMA_DISABLED);
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

esp_err_t spi_device_polling_transmit(
    spi_device_handle_t handle,
    spi_transaction_t *transaction
)
{
    assert(handle == (spi_device_handle_t)(uintptr_t)0x1);
    assert(transaction->length == AD7606_SERIAL_RX_BYTES * 8U);
    assert(transaction->rx_buffer != NULL);
    if (last_transaction != NULL) {
        assert(last_transaction == transaction); /* reusable transaction */
        assert(last_rx_buffer == transaction->rx_buffer);
    }
    last_transaction = transaction;
    last_rx_buffer = transaction->rx_buffer;
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

int spi_get_freq_limit(bool gpio_is_used, int input_delay_ns)
{
    fake_freq_limit_calls++;
    fake_freq_limit_gpio_used = gpio_is_used;
    fake_freq_limit_input_delay_ns = input_delay_ns;
    return 12000000;
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
    for (int pin = 0; pin < FAKE_GPIO_COUNT; ++pin) {
        /* Reset/boot state: every pin a floating input, output register 0. */
        fake_pin_mode[pin] = GPIO_MODE_INPUT;
        fake_pin_level[pin] = 0;
        fake_pin_configured[pin] = 0;
    }
    cs_driven_low_events = 0;
    fake_busy_level = 0;
    fake_convst_pulses = 0U;
    fake_measured_busy_reads = 0U;
    fake_cycle_count = 0U;
    fake_spi_error = 0;
    fake_gpio_error = 0;
    fake_spi_initialize_calls = 0;
    fake_spi_free_calls = 0;
    fake_transmit_calls = 0;
    fake_freq_limit_calls = 0;
    fake_freq_limit_gpio_used = false;
    fake_freq_limit_input_delay_ns = 0;
    last_transaction = NULL;
    last_rx_buffer = NULL;
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
    assert(last_bus_config.max_transfer_sz == AD7606_SERIAL_RX_BYTES);
    assert(last_device_config.mode == AD7606_SPI_MODE);
    assert(last_device_config.clock_speed_hz == CONFIG_AD7606_SPI_CLOCK_HZ);
    assert(last_device_config.spics_io_num == GPIO_NUM_41);
    assert(last_device_config.queue_size == 1);
    assert(fake_transmit_calls == 1);
    uint32_t measured_tconv_ns = 0U;
    assert(ad7606_driver_get_measured_tconv_ns(&measured_tconv_ns) == ESP_OK);
    assert(measured_tconv_ns == 4000U);
    assert(fake_freq_limit_calls == 1);
    assert(fake_freq_limit_gpio_used);
    assert(fake_freq_limit_input_delay_ns == 36);

    ad7606_sample_t sample = { 0 };
    assert(ad7606_driver_read(NULL) == ESP_ERR_INVALID_ARG);
    assert(ad7606_driver_read(&sample) == ESP_OK);
    assert(fake_transmit_calls == 2);
    assert(sample.timestamp_us == 1139);
    for (size_t channel = 0; channel < AD7606_CHANNEL_COUNT; ++channel) {
        assert(sample.raw[channel] == (int16_t)((int)channel * 1000 - 3000));
    }
    assert(ad7606_raw_to_uv(-32768) == -5000000.0f);
    assert(ad7606_raw_to_uv(32767) > 4999840.0f);

    sample.timestamp_us = -1;
    assert(ad7606_driver_read_result(&sample) == ESP_OK);
    assert(sample.timestamp_us == 0);
    assert(fake_transmit_calls == 3);

    ad7606_driver_deinit();
    assert(fake_spi_free_calls == 1);
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
}

static void test_two_complement_boundary_codes_convert_to_documented_uv_scale(void)
{
    /* MAX-74: exact raw-code -> uV endpoints for the currently documented
     * hardware contract (+-5 V range, no oversampling). This proves the
     * software math matches AD7606_FULL_SCALE_UV / AD7606_CODES_PER_HALF_RANGE
     * for every two's-complement boundary; it does not (and cannot) prove
     * that the installed module is actually strapped for +-5 V/OS=000 --
     * that requires the physical measurement tracked separately in MAX-74. */
    assert(ad7606_raw_to_uv(0) == 0.0f);              /* 0x0000 */
    assert(ad7606_raw_to_uv(32767) == 4999847.5f);     /* 0x7FFF */
    assert(ad7606_raw_to_uv(-32768) == -5000000.0f);   /* 0x8000 */
    assert(ad7606_raw_to_uv(-1) == -152.587890625f);   /* 0xFFFF */

    float microvolts = 0.0f;
    for (uint8_t os = 1U; os <= 8U; os = (uint8_t)(os * 2U)) {
        assert(ad7606_raw_to_uv_for_config(-32768, os, 5000U, &microvolts));
        assert(microvolts == -5000000.0f);
        assert(ad7606_raw_to_uv_for_config(32767, os, 10000U, &microvolts));
        assert(microvolts > 9999600.0f && microvolts < 10000000.0f);
    }
    assert(!ad7606_raw_to_uv_for_config(1, 3U, 5000U, &microvolts));
    assert(!ad7606_raw_to_uv_for_config(1, 1U, 500U, &microvolts));
    assert(!ad7606_raw_to_uv_for_config(1, 1U, 5000U, NULL));
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
    assert(observation.rx[0] == 0x00U && observation.rx[7] == 0x00U);
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


/* MAX-242: pin safety across the driver lifecycle. */
static void test_init_never_drives_cs_low_and_sets_safe_levels(void)
{
    reset_fake();
    assert(ad7606_driver_init() == ESP_OK);
    assert(cs_driven_low_events == 0);
    assert(fake_pin_mode[GPIO_NUM_41] == GPIO_MODE_OUTPUT && fake_pin_level[GPIO_NUM_41] == 1);
    assert(fake_pin_mode[GPIO_NUM_2] == GPIO_MODE_OUTPUT);
    assert(fake_pin_mode[GPIO_NUM_42] == GPIO_MODE_OUTPUT && fake_pin_level[GPIO_NUM_42] == 0);
    assert(fake_pin_mode[GPIO_NUM_47] == GPIO_MODE_INPUT);
    ad7606_driver_deinit();
}

static void test_deinit_releases_every_ad7606_pin_to_hi_z(void)
{
    reset_fake();
    assert(ad7606_driver_init() == ESP_OK);
    ad7606_driver_deinit();
    assert_ad7606_pins_hi_z();
    assert(cs_driven_low_events == 0);
    assert(ad7606_driver_read(NULL) == ESP_ERR_INVALID_STATE);
}

static void test_init_failure_leaves_pins_hi_z(void)
{
    reset_fake();
    fake_spi_error = 1;
    assert(ad7606_driver_init() != ESP_OK);
    fake_spi_error = 0;
    assert_ad7606_pins_hi_z();
    assert(cs_driven_low_events == 0);
}

static void test_safe_state_is_callable_any_time(void)
{
    reset_fake();
    ad7606_driver_enter_safe_state();
    assert_ad7606_pins_hi_z();
    assert(cs_driven_low_events == 0);
}

int main(void)
{
    test_init_never_drives_cs_low_and_sets_safe_levels();
    test_deinit_releases_every_ad7606_pin_to_hi_z();
    test_init_failure_leaves_pins_hi_z();
    test_safe_state_is_callable_any_time();
    test_successful_init_and_conversion();
    test_two_complement_boundary_codes_convert_to_documented_uv_scale();
    test_spi_error_does_not_publish_sample();
    test_constant_payload_is_observable_but_not_transport_failure();
    test_busy_timeout_fails_initialization_and_releases_bus();
    test_spi_initialization_error_is_returned();
    puts("AD7606 driver host tests passed");
    return 0;
}
