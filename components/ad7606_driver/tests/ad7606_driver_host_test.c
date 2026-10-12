#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ad7606_driver.h"
#include "ad7606_tconv_calibration.h"
#include "ad7606_tconv_limits.h"
#include "board_config.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    FAKE_TCONV_NORMAL,
    FAKE_TCONV_BUSY_NOT_LOW,
    FAKE_TCONV_NO_RISING_EDGE,
    FAKE_TCONV_NO_FALLING_EDGE,
    FAKE_TCONV_PREEMPT_FIRST_ATTEMPT,
} fake_tconv_scenario_t;

#define FAKE_CPU_HZ 160000000U
#define FAKE_GPIO_SET_CYCLES 160U
#define FAKE_GPIO_GET_CYCLES 96U
#define FAKE_CYCLE_READ_CYCLES 8U
#define FAKE_DELAY_CALL_CYCLES 40U
#define FAKE_TCONV_RISE_DELAY_CYCLES 16U
/* BUSY-high duration; the rising delay above is added to form its end time. */
#define FAKE_TCONV_BUSY_WIDTH_CYCLES 704U
#define FAKE_PREEMPT_DELAY_CYCLES (FAKE_CPU_HZ * 5U / 1000000U)
#define FAKE_POST_FALL_DIVISION_CYCLES 400U

#define FAKE_LOG_CAPACITY 16384U
static char fake_log_capture[FAKE_LOG_CAPACITY];

void ad7606_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    const size_t used = strlen(fake_log_capture);
    if (used >= sizeof(fake_log_capture) - 1U) {
        return;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(
        fake_log_capture + used,
        sizeof(fake_log_capture) - used,
        format,
        args);
    va_end(args);

    if (written > 0) {
        const size_t new_used = strlen(fake_log_capture);
        if (new_used < sizeof(fake_log_capture) - 1U) {
            fake_log_capture[new_used] = '\n';
            fake_log_capture[new_used + 1U] = '\0';
        }
    }
}

static int fake_busy_level;
static unsigned fake_convst_pulses;
static unsigned fake_convst_rises;
static unsigned fake_first_conversion_busy_reads;
static fake_tconv_scenario_t fake_tconv_scenario;
static uint32_t fake_cycle_count;
static uint32_t fake_convst_rise_cycles;
static uint32_t fake_pending_division_cycles;
static uint32_t fake_tconv_width_samples[AD7606_TCONV_MEASUREMENT_COUNT];
static uint32_t fake_current_tconv_width_cycles;
static uint32_t fake_gpio_get_cycles;
static unsigned fake_measurement_index;
static bool fake_tconv_width_samples_enabled;
static int fake_tconv_rise_observed;
static int fake_spi_error;
static int fake_gpio_error;
static bool fake_fail_measurement_convst_rise;
static bool fake_fail_measurement_convst_fall;
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
static unsigned fake_critical_enters;
static unsigned fake_critical_exits;
static unsigned fake_critical_depth;
static uint32_t fake_critical_start_cycles;
static uint32_t fake_critical_max_cycles;

void ad7606_test_port_enter_critical(portMUX_TYPE *mux)
{
    assert(mux != NULL);
    assert(fake_critical_depth == 0U);
    ++fake_critical_enters;
    ++fake_critical_depth;
    fake_critical_start_cycles = fake_cycle_count;
}

void ad7606_test_port_exit_critical(portMUX_TYPE *mux)
{
    assert(mux != NULL);
    assert(fake_critical_depth == 1U);
    const uint32_t elapsed_cycles = fake_cycle_count - fake_critical_start_cycles;
    if (elapsed_cycles > fake_critical_max_cycles) {
        fake_critical_max_cycles = elapsed_cycles;
    }
    ++fake_critical_exits;
    --fake_critical_depth;
}

static void assert_critical_balanced(void)
{
    assert(fake_critical_depth == 0U);
    assert(fake_critical_enters == fake_critical_exits);
}

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
    fake_cycle_count += FAKE_GPIO_SET_CYCLES;
    if (fake_gpio_error) {
        return ESP_ERR_INVALID_STATE;
    }
    assert(gpio_num >= 0 && gpio_num < FAKE_GPIO_COUNT);
    const int previous_level = fake_pin_level[gpio_num];
    if (gpio_num == BOARD_AD7606_CONVST_GPIO && level != 0U &&
        fake_convst_rises >= 1U && fake_fail_measurement_convst_rise) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gpio_num == BOARD_AD7606_CONVST_GPIO && level == 0U &&
        fake_convst_rises >= 2U && fake_fail_measurement_convst_fall) {
        return ESP_ERR_INVALID_STATE;
    }
    bool convst_rose_this_call = false;
    if (gpio_num == BOARD_AD7606_CONVST_GPIO && previous_level == 0 && level != 0U) {
        ++fake_convst_rises;
        convst_rose_this_call = true;
        fake_convst_rise_cycles = fake_cycle_count;
        fake_tconv_rise_observed = 0;
        if (fake_convst_rises > 1U) {
            if (fake_tconv_width_samples_enabled &&
                fake_measurement_index < AD7606_TCONV_MEASUREMENT_COUNT) {
                fake_current_tconv_width_cycles =
                    fake_tconv_width_samples[fake_measurement_index++];
            }
            else {
                fake_current_tconv_width_cycles = FAKE_TCONV_BUSY_WIDTH_CYCLES;
            }
        }
    }
    if (gpio_num == BOARD_AD7606_CONVST_GPIO && fake_pin_level[gpio_num] != 0 && level == 0U) {
        ++fake_convst_pulses;
        if (fake_convst_rises >= 2U && !fake_tconv_rise_observed) {
            /* Model the old 64-bit timeout division before its first BUSY sample. */
            fake_pending_division_cycles += FAKE_POST_FALL_DIVISION_CYCLES;
        }
    }
    fake_pin_level[gpio_num] = level != 0U ? 1 : 0;
    if (convst_rose_this_call &&
        fake_tconv_scenario == FAKE_TCONV_PREEMPT_FIRST_ATTEMPT &&
        fake_convst_rises == 2U) {
        /* A missed BUSY pulse when polling starts more than 4.4 us late. */
        fake_cycle_count += FAKE_PREEMPT_DELAY_CYCLES;
    }
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
    fake_cycle_count += fake_gpio_get_cycles;
    if (gpio_num != BOARD_AD7606_BUSY_GPIO) {
        return fake_busy_level;
    }

    if (fake_convst_rises == 0U) {
        return fake_tconv_scenario == FAKE_TCONV_BUSY_NOT_LOW ? 1 : fake_busy_level;
    }

    if (fake_convst_rises == 1U) {
        if (fake_tconv_scenario == FAKE_TCONV_BUSY_NOT_LOW && fake_convst_pulses == 1U) {
            const unsigned read_index = fake_first_conversion_busy_reads++;
            return read_index < 2U ? 0 : 1;
        }
        const uint32_t elapsed_cycles = fake_cycle_count - fake_convst_rise_cycles;
        return elapsed_cycles >= FAKE_TCONV_RISE_DELAY_CYCLES &&
                elapsed_cycles < FAKE_TCONV_RISE_DELAY_CYCLES +
                    fake_current_tconv_width_cycles
            ? 1
            : 0;
    }

    if (fake_tconv_scenario == FAKE_TCONV_NO_RISING_EDGE) {
        return 0;
    }
    if (fake_tconv_scenario == FAKE_TCONV_NO_FALLING_EDGE) {
        fake_tconv_rise_observed = fake_pin_level[BOARD_AD7606_CONVST_GPIO] != 0;
        return 1;
    }

    const uint32_t elapsed_cycles = fake_cycle_count - fake_convst_rise_cycles;
    const int busy_level = elapsed_cycles >= FAKE_TCONV_RISE_DELAY_CYCLES &&
            elapsed_cycles < FAKE_TCONV_RISE_DELAY_CYCLES +
                fake_current_tconv_width_cycles
        ? 1
        : 0;
    if (fake_pin_level[BOARD_AD7606_CONVST_GPIO] != 0 && busy_level != 0) {
        fake_tconv_rise_observed = 1;
    }
    return busy_level;
}

uint32_t esp_cpu_get_cycle_count(void)
{
    fake_cycle_count += FAKE_CYCLE_READ_CYCLES;
    fake_cycle_count += fake_pending_division_cycles;
    fake_pending_division_cycles = 0U;
    return fake_cycle_count;
}

int esp_clk_cpu_freq(void)
{
    return (int)FAKE_CPU_HZ;
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
    fake_cycle_count += us * (FAKE_CPU_HZ / 1000000U) + FAKE_DELAY_CALL_CYCLES;
}

static void reset_fake(void)
{
    assert_critical_balanced();
    for (int pin = 0; pin < FAKE_GPIO_COUNT; ++pin) {
        /* Reset/boot state: every pin a floating input, output register 0. */
        fake_pin_mode[pin] = GPIO_MODE_INPUT;
        fake_pin_level[pin] = 0;
        fake_pin_configured[pin] = 0;
    }
    cs_driven_low_events = 0;
    fake_busy_level = 0;
    fake_convst_pulses = 0U;
    fake_convst_rises = 0U;
    fake_first_conversion_busy_reads = 0U;
    fake_tconv_scenario = FAKE_TCONV_NORMAL;
    fake_cycle_count = 0U;
    fake_convst_rise_cycles = 0U;
    fake_pending_division_cycles = 0U;
    memset(fake_tconv_width_samples, 0, sizeof(fake_tconv_width_samples));
    fake_current_tconv_width_cycles = FAKE_TCONV_BUSY_WIDTH_CYCLES;
    fake_gpio_get_cycles = FAKE_GPIO_GET_CYCLES;
    fake_measurement_index = 0U;
    fake_tconv_width_samples_enabled = false;
    fake_tconv_rise_observed = 0;
    fake_spi_error = 0;
    fake_gpio_error = 0;
    fake_fail_measurement_convst_rise = false;
    fake_fail_measurement_convst_fall = false;
    fake_spi_initialize_calls = 0;
    fake_spi_free_calls = 0;
    fake_transmit_calls = 0;
    fake_freq_limit_calls = 0;
    fake_freq_limit_gpio_used = false;
    fake_freq_limit_input_delay_ns = 0;
    fake_log_capture[0] = '\0';
    fake_critical_enters = 0U;
    fake_critical_exits = 0U;
    fake_critical_depth = 0U;
    fake_critical_start_cycles = 0U;
    fake_critical_max_cycles = 0U;
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
    assert(measured_tconv_ns >= 4000U && measured_tconv_ns <= 5000U);
    assert(fake_freq_limit_calls == 1);
    assert(fake_freq_limit_gpio_used);
    assert(fake_freq_limit_input_delay_ns == 36);

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

    sample.timestamp_us = -1;
    assert(ad7606_driver_read_result(&sample) == ESP_OK);
    assert(sample.timestamp_us == 0);
    assert(fake_transmit_calls == 3);

    ad7606_driver_deinit();
    assert(fake_spi_free_calls == 1);
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
}


static unsigned s_max435_failures;

static void test_max435_tconv_measurement_subtracts_gpio_read_overhead(void)
{
    reset_fake();
    const esp_err_t init_result = ad7606_driver_init();
    uint32_t measured_tconv_ns = 0U;
    const esp_err_t measure_result = init_result == ESP_OK
        ? ad7606_driver_get_measured_tconv_ns(&measured_tconv_ns)
        : init_result;
    /* A 600 ns fake GPIO read bounds one poll phase; no additive bias is applied. */
    const bool passed = measure_result == ESP_OK &&
        measured_tconv_ns >= 4000U && measured_tconv_ns <= 4650U &&
        strstr(fake_log_capture, "overhead_ns=1200") != NULL &&
        strstr(fake_log_capture, "calibration_ns=0") != NULL &&
        strstr(fake_log_capture, "retries=0") != NULL;
    if (init_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    if (passed) {
        puts("MAX435 T1 PASS");
        fflush(stdout);
    }
    else {
        ++s_max435_failures;
        printf("MAX435 T1 FAIL init=%d measure=%d measured_ns=%u\n",
            (int)init_result,
            (int)measure_result,
            (unsigned)measured_tconv_ns);
        printf("MAX435 T1 LOG %s\n", fake_log_capture);
        fflush(stdout);
    }
}


static void test_max435_median_of_three_discards_single_outlier(void)
{
    reset_fake();
    fake_tconv_width_samples[0] = 1070U; /* 6688 ns preempted-sample outlier. */
    fake_tconv_width_samples[1] = 720U;  /* 4500 ns. */
    fake_tconv_width_samples[2] = 720U;  /* 4500 ns. */
    fake_tconv_width_samples_enabled = true;
    const esp_err_t init_result = ad7606_driver_init();
    uint32_t measured_tconv_ns = 0U;
    const esp_err_t measure_result = init_result == ESP_OK
        ? ad7606_driver_get_measured_tconv_ns(&measured_tconv_ns)
        : init_result;
    const bool passed = measure_result == ESP_OK &&
        measured_tconv_ns >= 4000U && measured_tconv_ns <= 4650U &&
        strstr(fake_log_capture, "AD7606_TCONV_SAMPLES:") != NULL &&
        strstr(fake_log_capture, "samples=3") != NULL &&
        strstr(fake_log_capture, "calibration_ns=0") != NULL;
    if (init_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    if (passed) {
        puts("MAX435 T5 PASS");
        fflush(stdout);
    }
    else {
        ++s_max435_failures;
        printf("MAX435 T5 FAIL init=%d measure=%d measured_ns=%u\n",
            (int)init_result,
            (int)measure_result,
            (unsigned)measured_tconv_ns);
        printf("MAX435 T5 LOG %s\n", fake_log_capture);
        fflush(stdout);
    }
}

static void test_tconv_rise_is_captured_while_convst_is_high(void)
{
    reset_fake();
    assert(ad7606_driver_init() == ESP_OK);
    assert(fake_convst_pulses == 1U + AD7606_TCONV_MEASUREMENT_COUNT);
    assert(fake_tconv_rise_observed);
    assert_critical_balanced();

    uint32_t measured_tconv_ns = 0U;
    assert(ad7606_driver_get_measured_tconv_ns(&measured_tconv_ns) == ESP_OK);
    assert(measured_tconv_ns >= 4000U && measured_tconv_ns <= 5000U);
    assert(strstr(fake_log_capture, "AD7606_TCONV_MEASURED: ") != NULL);
    assert(strstr(fake_log_capture, "AD7606_TCONV_IMPLAUSIBLE") == NULL);
    ad7606_driver_deinit();
}


static void test_tconv_preempted_first_attempt_is_retried(void)
{
    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_PREEMPT_FIRST_ATTEMPT;
    const esp_err_t init_result = ad7606_driver_init();
    uint32_t measured_tconv_ns = 0U;
    const esp_err_t measure_result = init_result == ESP_OK
        ? ad7606_driver_get_measured_tconv_ns(&measured_tconv_ns)
        : init_result;
    const bool passed = init_result == ESP_OK && measure_result == ESP_OK &&
        measured_tconv_ns >= 4000U && measured_tconv_ns <= 4650U &&
        fake_convst_pulses == 5U && fake_convst_rises == 5U &&
        strstr(fake_log_capture, "retries=1") != NULL &&
        fake_critical_enters == 4U && fake_critical_exits == 4U;
    if (init_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    assert_critical_balanced();
    if (passed) {
        puts("MAX435 preempt retry PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 preempt retry FAIL init=%d measure=%d measured=%u rises=%u pulses=%u enters=%u exits=%u\n",
            (int)init_result, (int)measure_result, (unsigned)measured_tconv_ns,
            fake_convst_rises, fake_convst_pulses,
            fake_critical_enters, fake_critical_exits);
    }
}


static void test_tconv_critical_section_balanced_on_gpio_failures(void)
{
    reset_fake();
    fake_fail_measurement_convst_rise = true;
    const esp_err_t rise_result = ad7606_driver_init();
    const bool rise_balanced = rise_result == ESP_ERR_INVALID_STATE &&
        fake_critical_enters == 1U && fake_critical_exits == 1U &&
        fake_critical_depth == 0U;
    const unsigned rise_enters = fake_critical_enters;
    const unsigned rise_exits = fake_critical_exits;
    assert_critical_balanced();
    ad7606_driver_deinit();

    reset_fake();
    fake_fail_measurement_convst_fall = true;
    const esp_err_t fall_result = ad7606_driver_init();
    const bool fall_balanced = fall_result == ESP_ERR_INVALID_STATE &&
        fake_critical_enters == 1U && fake_critical_exits == 1U &&
        fake_critical_depth == 0U;
    const unsigned fall_enters = fake_critical_enters;
    const unsigned fall_exits = fake_critical_exits;
    assert_critical_balanced();
    ad7606_driver_deinit();
    if (rise_balanced && fall_balanced) {
        puts("MAX435 critical balance PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 critical balance FAIL rise=%d fall=%d rise_counts=%u/%u fall_counts=%u/%u\n",
            (int)rise_result, (int)fall_result,
            rise_enters, rise_exits, fall_enters, fall_exits);
    }
}


static void tol_4_critical_section_duration_stays_within_budget(void)
{
    const uint32_t maximum_cycles = AD7606_TCONV_CRITICAL_MAX_US *
        (FAKE_CPU_HZ / 1000000U);

    reset_fake();
    const esp_err_t success_result = ad7606_driver_init();
    const bool success_bounded = success_result == ESP_OK &&
        fake_critical_max_cycles <= maximum_cycles &&
        fake_critical_enters > 0U &&
        fake_critical_enters == fake_critical_exits;
    const uint32_t success_max_cycles = fake_critical_max_cycles;
    if (success_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    assert_critical_balanced();

    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_PREEMPT_FIRST_ATTEMPT;
    const esp_err_t retry_result = ad7606_driver_init();
    const bool retry_bounded = retry_result == ESP_OK &&
        strstr(fake_log_capture, "retries=1") != NULL &&
        fake_critical_max_cycles <= maximum_cycles &&
        fake_critical_enters > 0U &&
        fake_critical_enters == fake_critical_exits;
    const uint32_t retry_max_cycles = fake_critical_max_cycles;
    if (retry_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    assert_critical_balanced();

    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_NO_RISING_EDGE;
    const esp_err_t timeout_result = ad7606_driver_init();
    const bool timeout_bounded = timeout_result == ESP_ERR_TIMEOUT &&
        fake_critical_max_cycles <= maximum_cycles &&
        fake_critical_enters > 0U &&
        fake_critical_enters == fake_critical_exits;
    const uint32_t timeout_max_cycles = fake_critical_max_cycles;
    assert_critical_balanced();
    ad7606_driver_deinit();

    if (success_bounded && retry_bounded && timeout_bounded) {
        puts("MAX435 TOL-4 PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 TOL-4 FAIL success=%d retry=%d timeout=%d max_us=%u/%u/%u budget=%u\n",
            success_bounded ? 1 : 0,
            retry_bounded ? 1 : 0,
            timeout_bounded ? 1 : 0,
            (unsigned)(success_max_cycles / (FAKE_CPU_HZ / 1000000U)),
            (unsigned)(retry_max_cycles / (FAKE_CPU_HZ / 1000000U)),
            (unsigned)(timeout_max_cycles / (FAKE_CPU_HZ / 1000000U)),
            (unsigned)AD7606_TCONV_CRITICAL_MAX_US);
    }
}


static void tol_5_timeout_retry_count_and_total_budget_are_pinned(void)
{
    assert(AD7606_TCONV_EDGE_WAIT_TIMEOUT_US == 100U);
    assert(AD7606_TCONV_MAX_RETRIES == 3U);
    const uint32_t maximum_measurement_cycles = AD7606_TCONV_MEASUREMENT_MAX_US *
        (FAKE_CPU_HZ / 1000000U);

    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_NO_RISING_EDGE;
    const uint32_t start_cycles = fake_cycle_count;
    const esp_err_t result = ad7606_driver_init();
    const uint32_t elapsed_cycles = fake_cycle_count - start_cycles;
    const bool passed = result == ESP_ERR_TIMEOUT &&
        fake_convst_pulses == 5U &&
        fake_critical_enters == 4U &&
        fake_critical_exits == 4U &&
        strstr(fake_log_capture,
            "AD7606_TCONV_DIAG: stage=NO_RISING_EDGE retries=3") != NULL &&
        strstr(fake_log_capture, "timeout_us=100") != NULL &&
        elapsed_cycles <= maximum_measurement_cycles &&
        fake_critical_max_cycles >= AD7606_TCONV_EDGE_WAIT_TIMEOUT_US *
            (FAKE_CPU_HZ / 1000000U) &&
        fake_critical_max_cycles <= AD7606_TCONV_CRITICAL_MAX_US *
            (FAKE_CPU_HZ / 1000000U);
    assert_critical_balanced();
    ad7606_driver_deinit();

    if (passed) {
        puts("MAX435 TOL-5 PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 TOL-5 FAIL result=%d pulses=%u enters=%u/%u elapsed_us=%u budget_us=%u\n",
            (int)result,
            fake_convst_pulses,
            fake_critical_enters,
            fake_critical_exits,
            (unsigned)(elapsed_cycles / (FAKE_CPU_HZ / 1000000U)),
            (unsigned)AD7606_TCONV_MEASUREMENT_MAX_US);
    }
}


static uint32_t fake_tconv_width_cycles_from_ns(uint32_t tconv_ns)
{
    return (uint32_t)(((uint64_t)tconv_ns * FAKE_CPU_HZ +
        UINT64_C(500000000)) / UINT64_C(1000000000));
}


static uint32_t measure_fake_tconv_widths(const uint32_t widths_ns[3])
{
    reset_fake();
    /* A fast fake GPIO read isolates median/outlier behavior from poll cadence. */
    fake_gpio_get_cycles = 0U;
    for (size_t i = 0U; i < 3U; ++i) {
        fake_tconv_width_samples[i] = fake_tconv_width_cycles_from_ns(widths_ns[i]);
    }
    fake_tconv_width_samples_enabled = true;
    const esp_err_t init_result = ad7606_driver_init();
    uint32_t measured_ns = 0U;
    const esp_err_t measure_result = init_result == ESP_OK
        ? ad7606_driver_get_measured_tconv_ns(&measured_ns)
        : init_result;
    const bool complete = measure_result == ESP_OK && fake_measurement_index == 3U;
    if (init_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    return complete ? measured_ns : 0U;
}


static void tol_6_driver_discards_one_outlier_in_any_sample_position(void)
{
    static const uint32_t one_outlier_orders_ns[3][3] = {
        {6688U, 4500U, 4500U},
        {4500U, 6688U, 4500U},
        {4500U, 4500U, 6688U},
    };
    bool passed = true;
    for (size_t order = 0U; order < 3U; ++order) {
        passed = passed && measure_fake_tconv_widths(
            one_outlier_orders_ns[order]) == 4500U;
    }

    /* Two high samples are not a lone preemption artifact; never accept OS1. */
    const uint32_t two_outliers_ns[] = {6688U, 6700U, 4500U};
    const uint32_t two_outlier_result_ns =
        measure_fake_tconv_widths(two_outliers_ns);
    passed = passed && two_outlier_result_ns > 4650U;

    if (passed) {
        puts("MAX435 TOL-6 driver PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 TOL-6 driver FAIL two-outlier-result=%u\n",
            (unsigned)two_outlier_result_ns);
    }
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

static void test_busy_not_low_before_convst_timeout_is_diagnosed(void)
{
    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_BUSY_NOT_LOW;
    assert(ad7606_driver_init() == ESP_ERR_TIMEOUT);
    assert(fake_transmit_calls == 1);
    assert(fake_convst_pulses == 1U);
    assert(fake_spi_free_calls == 1);
    assert(strstr(fake_log_capture,
        "AD7606_TCONV_DIAG: stage=BUSY_NOT_LOW_BEFORE_CONVST") != NULL);
    assert(strstr(fake_log_capture, "timeout_us=100") != NULL);
    ad7606_sample_t sample = { 0 };
    assert(ad7606_driver_read(&sample) == ESP_ERR_INVALID_STATE);
    ad7606_driver_deinit();
}

static void test_no_rising_edge_timeout_is_diagnosed(void)
{
    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_NO_RISING_EDGE;
    const esp_err_t init_result = ad7606_driver_init();
    const bool passed = init_result == ESP_ERR_TIMEOUT &&
        fake_transmit_calls == 1 && fake_convst_pulses == 5U &&
        fake_spi_free_calls == 1 &&
        strstr(fake_log_capture,
            "AD7606_TCONV_DIAG: stage=NO_RISING_EDGE retries=3") != NULL &&
        strstr(fake_log_capture, "timeout_us=100") != NULL &&
        fake_critical_enters == 4U && fake_critical_exits == 4U;
    assert_critical_balanced();
    if (init_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    if (passed) {
        puts("MAX435 persistent no rising PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 persistent no rising FAIL init=%d pulses=%u enters=%u exits=%u\n",
            (int)init_result, fake_convst_pulses,
            fake_critical_enters, fake_critical_exits);
    }
    ad7606_driver_deinit();
}

static void test_no_falling_edge_timeout_is_diagnosed(void)
{
    reset_fake();
    fake_tconv_scenario = FAKE_TCONV_NO_FALLING_EDGE;
    const esp_err_t init_result = ad7606_driver_init();
    const bool passed = init_result == ESP_ERR_TIMEOUT &&
        fake_transmit_calls == 1 && fake_convst_pulses == 2U &&
        fake_spi_free_calls == 1 &&
        strstr(fake_log_capture,
            "AD7606_TCONV_DIAG: stage=NO_FALLING_EDGE") != NULL &&
        strstr(fake_log_capture, "timeout_us=100") != NULL &&
        fake_critical_enters == 1U && fake_critical_exits == 1U;
    assert_critical_balanced();
    if (init_result == ESP_OK) {
        ad7606_driver_deinit();
    }
    if (passed) {
        puts("MAX435 no falling edge PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 no falling edge FAIL init=%d pulses=%u enters=%u exits=%u\n",
            (int)init_result, fake_convst_pulses,
            fake_critical_enters, fake_critical_exits);
    }
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
#if defined(AD7606_TCONV_TOL_ONLY)
    tol_6_driver_discards_one_outlier_in_any_sample_position();
    tol_4_critical_section_duration_stays_within_budget();
    tol_5_timeout_retry_count_and_total_budget_are_pinned();
    return s_max435_failures == 0U ? 0 : 1;
#else
    test_max435_tconv_measurement_subtracts_gpio_read_overhead();
    test_max435_median_of_three_discards_single_outlier();
    test_tconv_rise_is_captured_while_convst_is_high();
    test_init_never_drives_cs_low_and_sets_safe_levels();
    test_deinit_releases_every_ad7606_pin_to_hi_z();
    test_init_failure_leaves_pins_hi_z();
    test_safe_state_is_callable_any_time();
    test_successful_init_and_conversion();
    test_two_complement_boundary_codes_convert_to_documented_uv_scale();
    test_spi_error_does_not_publish_sample();
    test_constant_payload_is_observable_but_not_transport_failure();
    test_busy_not_low_before_convst_timeout_is_diagnosed();
    test_tconv_preempted_first_attempt_is_retried();
    test_tconv_critical_section_balanced_on_gpio_failures();
    tol_6_driver_discards_one_outlier_in_any_sample_position();
    tol_4_critical_section_duration_stays_within_budget();
    tol_5_timeout_retry_count_and_total_budget_are_pinned();
    test_no_rising_edge_timeout_is_diagnosed();
    test_no_falling_edge_timeout_is_diagnosed();
    test_spi_initialization_error_is_returned();
    assert_critical_balanced();
    puts("AD7606 driver host tests passed");
    return s_max435_failures == 0U ? 0 : 1;
#endif
}
