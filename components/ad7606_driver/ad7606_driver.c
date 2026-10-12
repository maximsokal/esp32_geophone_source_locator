#include "ad7606_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "sdkconfig.h"
#include "ad7606_tconv_calibration.h"
#include "ad7606_tconv_limits.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"


static const char *TAG =
    "AD7606_DRIVER";


#define AD7606_SPI_INPUT_DELAY_ESTIMATE_NS       36

#define AD7606_RESET_PULSE_US                  10
#define AD7606_RESET_SETTLE_US                 20

#define AD7606_CONVST_PULSE_US                 1
#define AD7606_CONVERSION_GUARD_US             5
#define AD7606_POST_CONVERSION_DELAY_US        1

/*
 * Normal conversion itself is only a few microseconds.
 * 100 us gives generous margin for Stage 11B polling.
 */
#define AD7606_TCONV_CRITICAL_SECTION_MARGIN_US 49U

#if (AD7606_TCONV_EDGE_WAIT_TIMEOUT_US + AD7606_CONVST_PULSE_US + \
    AD7606_TCONV_CRITICAL_SECTION_MARGIN_US) > \
    AD7606_TCONV_CRITICAL_MAX_US
#error "AD7606 tCONV critical section exceeds its documented 150 us bound"
#endif

#define AD7606_CODES_PER_HALF_RANGE             32768.0f


static spi_device_handle_t s_spi_device =
    NULL;

/* No DMA: four 16-bit words fit in the SPI FIFO. Reused by the sole reader. */
static uint8_t s_spi_rx_buffer[AD7606_SERIAL_RX_BYTES];
static spi_transaction_t s_spi_transaction;

static bool s_spi_bus_initialized =
    false;

static bool s_initialized =
    false;

static bool s_busy_release_error_logged =
    false;

static uint32_t s_measured_tconv_ns = 0U;
static uint32_t s_measured_tconv_overhead_ns = 0U;
static uint32_t s_measured_tconv_retries = 0U;
static portMUX_TYPE s_tconv_measurement_mux = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    AD7606_TCONV_DIAG_STAGE_BUSY_NOT_LOW_BEFORE_CONVST,
    AD7606_TCONV_DIAG_STAGE_NO_RISING_EDGE,
    AD7606_TCONV_DIAG_STAGE_NO_FALLING_EDGE,
    AD7606_TCONV_DIAG_STAGE_OK,
} ad7606_tconv_diag_stage_t;

typedef struct {
    ad7606_tconv_diag_stage_t stage;
    int busy_at_fail;
    int busy_before_convst;
    uint32_t waited_cycles;
    uint32_t rise_after_cycles;
    uint32_t cpu_hz;
    uint32_t retries;
} ad7606_tconv_diag_t;

static ad7606_tconv_diag_t s_last_tconv_diag = {
    .stage = AD7606_TCONV_DIAG_STAGE_OK,
    .busy_at_fail = -1,
    .busy_before_convst = -1,
};

static ad7606_serial_observation_t s_last_serial_observation = {
    .pattern = AD7606_SERIAL_PATTERN_UNKNOWN,
};


/*
 * Wait until BUSY reaches the requested state.
 *
 * No logging is done here because this helper can be used in
 * microsecond-sensitive sections.
 */
static esp_err_t ad7606_wait_busy_level(
    int expected_level,
    int64_t timeout_us
)
{
    const int64_t deadline_us =
        esp_timer_get_time() +
        timeout_us;

    while (
        gpio_get_level(
            BOARD_AD7606_BUSY_GPIO
        ) != expected_level
    ) {
        if (
            esp_timer_get_time() >=
            deadline_us
        ) {
            return ESP_ERR_TIMEOUT;
        }

        esp_rom_delay_us(1);
    }

    return ESP_OK;
}


static esp_err_t ad7606_wait_busy_edge_cycles(
    int expected_level,
    uint32_t timeout_start_cycles,
    uint32_t timeout_cycles,
    uint32_t cycle_read_overhead_cycles,
    uint32_t *edge_cycles,
    uint32_t *gpio_read_overhead_cycles,
    uint32_t *elapsed_cycles_at_timeout
)
{
    for (;;) {
        const uint32_t poll_start_cycles =
            (uint32_t)esp_cpu_get_cycle_count();
        const int busy_level = gpio_get_level(BOARD_AD7606_BUSY_GPIO);
        const uint32_t poll_end_cycles =
            (uint32_t)esp_cpu_get_cycle_count();
        const uint32_t read_span_cycles =
            (uint32_t)(poll_end_cycles - poll_start_cycles);
        const uint32_t read_overhead_cycles =
            read_span_cycles > cycle_read_overhead_cycles
                ? read_span_cycles - cycle_read_overhead_cycles
                : 0U;

        if (busy_level == expected_level) {
            /*
             * GPIO is sampled during the call. Backdate the returned cycle
             * counter to the midpoint of that call, removing half of the
             * measured per-poll read latency at each edge.
             */
            *edge_cycles = poll_start_cycles + read_overhead_cycles / 2U;
            *gpio_read_overhead_cycles = read_overhead_cycles;
            return ESP_OK;
        }
        if ((uint32_t)(poll_end_cycles - timeout_start_cycles) >= timeout_cycles) {
            *elapsed_cycles_at_timeout =
                (uint32_t)(poll_end_cycles - timeout_start_cycles);
            return ESP_ERR_TIMEOUT;
        }
    }
}


static const char *ad7606_tconv_diag_stage_name(ad7606_tconv_diag_stage_t stage)
{
    switch (stage) {
    case AD7606_TCONV_DIAG_STAGE_BUSY_NOT_LOW_BEFORE_CONVST:
        return "BUSY_NOT_LOW_BEFORE_CONVST";
    case AD7606_TCONV_DIAG_STAGE_NO_RISING_EDGE:
        return "NO_RISING_EDGE";
    case AD7606_TCONV_DIAG_STAGE_NO_FALLING_EDGE:
        return "NO_FALLING_EDGE";
    case AD7606_TCONV_DIAG_STAGE_OK:
    default:
        return "OK";
    }
}


static uint64_t ad7606_cycles_to_ns(uint32_t cycles, uint32_t cpu_hz)
{
    if (cpu_hz == 0U) {
        return 0U;
    }
    return ((uint64_t)cycles * UINT64_C(1000000000) + cpu_hz / 2U) / cpu_hz;
}


static uint32_t ad7606_measure_cycle_counter_read_overhead_cycles(void)
{
    uint32_t samples[5] = {0U};
    for (size_t i = 0U; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const uint32_t start_cycles =
            (uint32_t)esp_cpu_get_cycle_count();
        const uint32_t end_cycles =
            (uint32_t)esp_cpu_get_cycle_count();
        samples[i] = (uint32_t)(end_cycles - start_cycles);
    }
    return ad7606_tconv_median_ns(samples, sizeof(samples) / sizeof(samples[0]));
}


static esp_err_t ad7606_measure_busy_tconv_once(
    uint32_t cpu_hz,
    uint32_t cycle_read_overhead_cycles,
    uint32_t *tconv_ns,
    uint32_t *gpio_read_overhead_ns,
    uint32_t *retries
)
{
    s_last_tconv_diag = (ad7606_tconv_diag_t) {
        .stage = AD7606_TCONV_DIAG_STAGE_OK,
        .busy_at_fail = -1,
        .busy_before_convst = -1,
    };

    s_last_tconv_diag.cpu_hz = cpu_hz;
    *retries = 0U;

    const uint64_t timeout_cycles_64 =
        (uint64_t)cpu_hz * AD7606_TCONV_EDGE_WAIT_TIMEOUT_US / 1000000U;
    const uint64_t convst_pulse_cycles_64 =
        ((uint64_t)cpu_hz * AD7606_CONVST_PULSE_US + 999999U) / 1000000U;
    if (timeout_cycles_64 == 0U || timeout_cycles_64 > UINT32_MAX ||
        convst_pulse_cycles_64 == 0U || convst_pulse_cycles_64 > UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t timeout_cycles = (uint32_t)timeout_cycles_64;
    const uint32_t convst_pulse_cycles = (uint32_t)convst_pulse_cycles_64;

    esp_err_t err = ad7606_wait_busy_level(0, AD7606_TCONV_EDGE_WAIT_TIMEOUT_US);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            s_last_tconv_diag.stage = AD7606_TCONV_DIAG_STAGE_BUSY_NOT_LOW_BEFORE_CONVST;
            s_last_tconv_diag.busy_at_fail = gpio_get_level(BOARD_AD7606_BUSY_GPIO);
            /* No CONVST was raised; retain the final pre-CONVST level sample. */
            s_last_tconv_diag.busy_before_convst = s_last_tconv_diag.busy_at_fail;
        }
        return err;
    }
    /* The wait helper returned on its existing BUSY LOW sample. */
    s_last_tconv_diag.busy_before_convst = 0;
    uint32_t retry_count = 0U;
    uint32_t busy_rising_cycles = 0U;
    uint32_t busy_falling_cycles = 0U;
    uint32_t rising_gpio_read_overhead_cycles = 0U;
    uint32_t falling_gpio_read_overhead_cycles = 0U;
    uint32_t elapsed_cycles_at_timeout = 0U;
    uint32_t convst_rise_cycles = 0U;

    for (;;) {
        bool rising_edge_seen = false;
        busy_rising_cycles = 0U;
        busy_falling_cycles = 0U;
        rising_gpio_read_overhead_cycles = 0U;
        falling_gpio_read_overhead_cycles = 0U;
        elapsed_cycles_at_timeout = 0U;

        /*
         * Keep CONVST and both BUSY edge captures on this core without task
         * preemption. The shared 100 us edge deadline, 1 us minimum pulse,
         * and one bounded final GPIO/cycle-counter poll fit within 150 us.
         * This section contains no logging, blocking calls, or RTOS calls.
         */
        portENTER_CRITICAL(&s_tconv_measurement_mux);
        const uint32_t measurement_start_cycles =
            (uint32_t)esp_cpu_get_cycle_count();
        err = gpio_set_level(BOARD_AD7606_CONVST_GPIO, 1);
        if (err == ESP_OK) {
            convst_rise_cycles = (uint32_t)esp_cpu_get_cycle_count();
            err = ad7606_wait_busy_edge_cycles(
                1, measurement_start_cycles, timeout_cycles,
                cycle_read_overhead_cycles, &busy_rising_cycles,
                &rising_gpio_read_overhead_cycles, &elapsed_cycles_at_timeout);
            rising_edge_seen = err == ESP_OK;

            while ((uint32_t)((uint32_t)esp_cpu_get_cycle_count() -
                convst_rise_cycles) < convst_pulse_cycles) {
            }

            const esp_err_t convst_fall_err =
                gpio_set_level(BOARD_AD7606_CONVST_GPIO, 0);
            if (convst_fall_err != ESP_OK) {
                err = convst_fall_err;
            }
            else if (rising_edge_seen) {
                err = ad7606_wait_busy_edge_cycles(
                    0, measurement_start_cycles, timeout_cycles,
                    cycle_read_overhead_cycles, &busy_falling_cycles,
                    &falling_gpio_read_overhead_cycles,
                    &elapsed_cycles_at_timeout);
            }
        }
        portEXIT_CRITICAL(&s_tconv_measurement_mux);

        if (err == ESP_OK) {
            break;
        }

        s_last_tconv_diag.retries = retry_count;
        *retries = retry_count;
        if (!rising_edge_seen && err == ESP_ERR_TIMEOUT) {
            s_last_tconv_diag.stage = AD7606_TCONV_DIAG_STAGE_NO_RISING_EDGE;
            s_last_tconv_diag.waited_cycles = elapsed_cycles_at_timeout;
            s_last_tconv_diag.busy_at_fail =
                gpio_get_level(BOARD_AD7606_BUSY_GPIO);
            if (retry_count >= AD7606_TCONV_MAX_RETRIES) {
                return err;
            }

            ++retry_count;
            err = ad7606_wait_busy_level(0, AD7606_TCONV_EDGE_WAIT_TIMEOUT_US);
            if (err != ESP_OK) {
                if (err == ESP_ERR_TIMEOUT) {
                    s_last_tconv_diag.stage =
                        AD7606_TCONV_DIAG_STAGE_BUSY_NOT_LOW_BEFORE_CONVST;
                    s_last_tconv_diag.busy_at_fail =
                        gpio_get_level(BOARD_AD7606_BUSY_GPIO);
                }
                s_last_tconv_diag.retries = retry_count;
                *retries = retry_count;
                return err;
            }
            s_last_tconv_diag.busy_before_convst = 0;
            continue;
        }

        if (rising_edge_seen && err == ESP_ERR_TIMEOUT) {
            s_last_tconv_diag.stage = AD7606_TCONV_DIAG_STAGE_NO_FALLING_EDGE;
            s_last_tconv_diag.waited_cycles = elapsed_cycles_at_timeout;
            s_last_tconv_diag.rise_after_cycles =
                (uint32_t)(busy_rising_cycles - convst_rise_cycles);
            s_last_tconv_diag.busy_at_fail =
                gpio_get_level(BOARD_AD7606_BUSY_GPIO);
        }
        return err;
    }

    const uint32_t width_cycles = busy_falling_cycles - busy_rising_cycles;
    const uint64_t raw_width_ns = ad7606_cycles_to_ns(width_cycles, cpu_hz);
    const uint64_t overhead_cycles =
        (uint64_t)rising_gpio_read_overhead_cycles +
        falling_gpio_read_overhead_cycles;
    const uint64_t overhead_ns_64 =
        (overhead_cycles * UINT64_C(1000000000) + cpu_hz / 2U) / cpu_hz;
    if (width_cycles == 0U || raw_width_ns > UINT32_MAX ||
        overhead_ns_64 > UINT32_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *gpio_read_overhead_ns = (uint32_t)overhead_ns_64;
    *tconv_ns = ad7606_tconv_apply_known_calibration_ns(
        (uint32_t)raw_width_ns);
    if (*tconv_ns == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_last_tconv_diag.stage = AD7606_TCONV_DIAG_STAGE_OK;
    s_last_tconv_diag.retries = retry_count;
    *retries = retry_count;
    return ESP_OK;
}


static esp_err_t ad7606_measure_busy_tconv(void)
{
    const int cpu_hz = esp_clk_cpu_freq();
    if (cpu_hz <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t cycle_read_overhead_cycles =
        ad7606_measure_cycle_counter_read_overhead_cycles();
    uint32_t tconv_samples_ns[AD7606_TCONV_MEASUREMENT_COUNT] = {0U};
    uint32_t overhead_samples_ns[AD7606_TCONV_MEASUREMENT_COUNT] = {0U};
    s_measured_tconv_retries = 0U;
    for (size_t i = 0U; i < AD7606_TCONV_MEASUREMENT_COUNT; ++i) {
        uint32_t sample_retries = 0U;
        const esp_err_t err = ad7606_measure_busy_tconv_once(
            (uint32_t)cpu_hz,
            cycle_read_overhead_cycles,
            &tconv_samples_ns[i],
            &overhead_samples_ns[i],
            &sample_retries);
        s_measured_tconv_retries += sample_retries;
        if (err != ESP_OK) {
            return err;
        }
    }

    s_measured_tconv_ns = ad7606_tconv_median_ns(
        tconv_samples_ns,
        AD7606_TCONV_MEASUREMENT_COUNT);
    s_measured_tconv_overhead_ns = ad7606_tconv_median_ns(
        overhead_samples_ns,
        AD7606_TCONV_MEASUREMENT_COUNT);
    ESP_LOGI(TAG,
        "AD7606_TCONV_SAMPLES: %u,%u,%u ns",
        (unsigned)tconv_samples_ns[0],
        (unsigned)tconv_samples_ns[1],
        (unsigned)tconv_samples_ns[2]);
    return s_measured_tconv_ns > 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}


/*
 * MAX-242: the output register is loaded BEFORE the pins become outputs.
 * gpio_config(OUTPUT) first would drive each pin from the register's reset
 * value (0) for a moment -- a LOW glitch on CS selects the ADC before the
 * firmware deselects it.
 */
static esp_err_t ad7606_configure_gpio(void)
{
    esp_err_t err = gpio_set_level(BOARD_AD7606_CS_GPIO, 1);
    if (err == ESP_OK) {
        err = gpio_set_level(BOARD_AD7606_RESET_GPIO, 0);
    }
    if (err == ESP_OK) {
        err = gpio_set_level(BOARD_AD7606_CONVST_GPIO, 0);
    }
    if (err != ESP_OK) {
        return err;
    }

    const gpio_config_t output_config = {
        .pin_bit_mask =
            (1ULL << BOARD_AD7606_RESET_GPIO) |
            (1ULL << BOARD_AD7606_CONVST_GPIO) |
            (1ULL << BOARD_AD7606_CS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&output_config);
    if (err != ESP_OK) {
        return err;
    }

    const gpio_config_t busy_config = {
        .pin_bit_mask = (1ULL << BOARD_AD7606_BUSY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&busy_config);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "GPIO configured: RESET=%d CONVST=%d BUSY=%d CS=%d",
        BOARD_AD7606_RESET_GPIO,
        BOARD_AD7606_CONVST_GPIO,
        BOARD_AD7606_BUSY_GPIO,
        BOARD_AD7606_CS_GPIO
    );

    return ESP_OK;
}


void ad7606_driver_enter_safe_state(void)
{
    /*
     * MAX-242: every ESP32 pin that can drive the AD7606 becomes a Hi-Z
     * input with no internal pull. CS is then held deselected by the
     * external 10 kOhm pull-up to the AD7606's own VIO, which disappears
     * together with the AD7606 +5 V rail -- so an unpowered AD7606 is never
     * back-fed through an actively driven ESP32 output.
     */
    const gpio_config_t safe_config = {
        .pin_bit_mask =
            (1ULL << BOARD_AD7606_RESET_GPIO) |
            (1ULL << BOARD_AD7606_CONVST_GPIO) |
            (1ULL << BOARD_AD7606_CS_GPIO) |
            (1ULL << BOARD_AD7606_SPI_SCLK_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&safe_config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not release AD7606 pins to Hi-Z");
    }
}


static esp_err_t ad7606_init_spi_bus(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num =
            -1,

        .miso_io_num =
            BOARD_AD7606_SPI_MISO_GPIO,

        .sclk_io_num =
            BOARD_AD7606_SPI_SCLK_GPIO,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .data4_io_num =
            -1,

        .data5_io_num =
            -1,

        .data6_io_num =
            -1,

        .data7_io_num =
            -1,

        .max_transfer_sz =
            AD7606_SERIAL_RX_BYTES,

        .flags =
            SPICOMMON_BUSFLAG_MASTER,

        .intr_flags =
            0,
    };

    ESP_LOGI(
        TAG,
        "Initializing dedicated SPI3: MISO=%d SCLK=%d",
        BOARD_AD7606_SPI_MISO_GPIO,
        BOARD_AD7606_SPI_SCLK_GPIO
    );

    esp_err_t err =
        spi_bus_initialize(
            BOARD_AD7606_SPI_HOST,
            &bus_config,
            SPI_DMA_DISABLED
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI3 bus initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_spi_bus_initialized =
        true;

    ESP_LOGI(
        TAG,
        "SPI3 initialized successfully without DMA; RX=%u bytes",
        (unsigned)sizeof(s_spi_rx_buffer)
    );

    return ESP_OK;
}


static esp_err_t ad7606_add_spi_device(void)
{
    const spi_device_interface_config_t config = {
        .command_bits =
            0,

        .address_bits =
            0,

        .dummy_bits =
            0,

        /*
         * AD7606 serial timing:
         * SCLK idle HIGH, data sampled on falling edge.
         */
        .mode =
            2,

        .clock_speed_hz =
            CONFIG_AD7606_SPI_CLOCK_HZ,

        .spics_io_num =
            BOARD_AD7606_CS_GPIO,

        .queue_size =
            1,
    };

    esp_err_t err =
        spi_bus_add_device(
            BOARD_AD7606_SPI_HOST,
            &config,
            &s_spi_device
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "SPI device registered: SPI3 mode=2 clock=%d Hz CS=%d",
            CONFIG_AD7606_SPI_CLOCK_HZ,
            BOARD_AD7606_CS_GPIO
        );
    }

    return err;
}


static void ad7606_release_spi(void)
{
    if (s_spi_device != NULL) {
        esp_err_t err =
            spi_bus_remove_device(
                s_spi_device
            );

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "SPI3 device removal failed: %s",
                esp_err_to_name(err)
            );
        }

        s_spi_device =
            NULL;
    }

    if (s_spi_bus_initialized) {
        esp_err_t err =
            spi_bus_free(
                BOARD_AD7606_SPI_HOST
            );

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "SPI3 bus free failed: %s",
                esp_err_to_name(err)
            );
        }
        else {
            s_spi_bus_initialized =
                false;
        }
    }
}


/*
 * Hardware RESET only.
 *
 * BUSY is deliberately NOT used to judge whether RESET
 * succeeded. We only generate the required RESET pulse and
 * leave a generous settling interval.
 */
static esp_err_t ad7606_hardware_reset(void)
{
    esp_err_t err =
        gpio_set_level(
            BOARD_AD7606_RESET_GPIO,
            0
        );

    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(1);

    err =
        gpio_set_level(
            BOARD_AD7606_RESET_GPIO,
            1
        );

    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(
        AD7606_RESET_PULSE_US
    );

    err =
        gpio_set_level(
            BOARD_AD7606_RESET_GPIO,
            0
        );

    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(
        AD7606_RESET_SETTLE_US
    );

    ESP_LOGI(
        TAG,
        "Hardware RESET complete: pulse=%d us settle=%d us",
        AD7606_RESET_PULSE_US,
        AD7606_RESET_SETTLE_US
    );

    return ESP_OK;
}


/* One reusable, FIFO-sized, four-word DOUTA polling transaction. */
static esp_err_t ad7606_spi_read_words(ad7606_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_spi_rx_buffer, 0, sizeof(s_spi_rx_buffer));
    s_spi_transaction.length = sizeof(s_spi_rx_buffer) * 8U;
    s_spi_transaction.rx_buffer = s_spi_rx_buffer;
    const esp_err_t err = spi_device_polling_transmit(
        s_spi_device,
        &s_spi_transaction
    );
    if (err != ESP_OK) {
        return err;
    }

    memcpy(s_last_serial_observation.rx, s_spi_rx_buffer, sizeof(s_spi_rx_buffer));
    s_last_serial_observation.pattern = ad7606_serial_classify(s_spi_rx_buffer);
    s_last_serial_observation.spi_transaction_ok = true;
    if (!ad7606_serial_decode_frame(s_spi_rx_buffer, sample->raw)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}


/* One-shot conversion retained only for driver self-test and direct probes. */
static esp_err_t ad7606_read_once(ad7606_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sample->timestamp_us = 0;
    s_last_serial_observation = (ad7606_serial_observation_t) {
        .pattern = AD7606_SERIAL_PATTERN_UNKNOWN,
    };

    esp_err_t err = gpio_set_level(BOARD_AD7606_CONVST_GPIO, 1);
    if (err != ESP_OK) {
        return err;
    }
    sample->timestamp_us = esp_timer_get_time();
    esp_rom_delay_us(AD7606_CONVST_PULSE_US);
    err = gpio_set_level(BOARD_AD7606_CONVST_GPIO, 0);
    if (err != ESP_OK) {
        return err;
    }

    esp_rom_delay_us(AD7606_CONVERSION_GUARD_US);
    if (gpio_get_level(BOARD_AD7606_BUSY_GPIO) != 0) {
        err = ad7606_wait_busy_level(0, AD7606_TCONV_EDGE_WAIT_TIMEOUT_US);
        if (err != ESP_OK) {
            if (!s_busy_release_error_logged) {
                ESP_LOGE(TAG, "BUSY remained HIGH after conversion; level=%d",
                    gpio_get_level(BOARD_AD7606_BUSY_GPIO));
                s_busy_release_error_logged = true;
            }
            return err;
        }
    }

    esp_rom_delay_us(AD7606_POST_CONVERSION_DELAY_US);
    s_last_serial_observation.conversion_complete = true;
    s_last_serial_observation.busy_level = gpio_get_level(BOARD_AD7606_BUSY_GPIO);
    return ad7606_spi_read_words(sample);
}


static esp_err_t ad7606_self_test(void)
{
    /*
     * Do NOT gate the first conversion on BUSY being LOW.
     *
     * On this HW-AD7606-F4 module BUSY can be HIGH after RESET,
     * while the first real CONVST + SPI transaction still works
     * correctly.
     */
    const int busy_before_first_conversion =
        gpio_get_level(
            BOARD_AD7606_BUSY_GPIO
        );

    ESP_LOGI(
        TAG,
        "Self-test starting: BUSY before first CONVST=%d",
        busy_before_first_conversion
    );

    ad7606_sample_t sample = {0};

    esp_err_t err =
        ad7606_read_once(
            &sample
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Self-test conversion failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /* Measure a second clean pulse because BUSY may be high after RESET. */
    err = ad7606_measure_busy_tconv();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Self-test BUSY tCONV measurement failed: %s",
            esp_err_to_name(err));
        ESP_LOGE(TAG,
            "AD7606_TCONV_DIAG: stage=%s retries=%u busy_at_fail=%d busy_before_convst=%d rise_after_ns=%llu waited_ns=%llu timeout_us=%u",
            ad7606_tconv_diag_stage_name(s_last_tconv_diag.stage),
            (unsigned)s_last_tconv_diag.retries,
            s_last_tconv_diag.busy_at_fail,
            s_last_tconv_diag.busy_before_convst,
            (unsigned long long)ad7606_cycles_to_ns(
                s_last_tconv_diag.rise_after_cycles,
                s_last_tconv_diag.cpu_hz),
            (unsigned long long)ad7606_cycles_to_ns(
                s_last_tconv_diag.waited_cycles,
                s_last_tconv_diag.cpu_hz),
              (unsigned)AD7606_TCONV_EDGE_WAIT_TIMEOUT_US);
        return err;
    }
    ESP_LOGI(TAG,
        "AD7606_TCONV_MEASURED: %u ns overhead_ns=%u calibration_ns=%d samples=%u retries=%u",
        (unsigned)s_measured_tconv_ns,
        (unsigned)s_measured_tconv_overhead_ns,
        (int)AD7606_TCONV_MEASUREMENT_CALIBRATION_NS,
        (unsigned)AD7606_TCONV_MEASUREMENT_COUNT,
        (unsigned)s_measured_tconv_retries);
    if (s_measured_tconv_ns < 1000U || s_measured_tconv_ns > 20000U) {
        ESP_LOGW(TAG, "AD7606_TCONV_IMPLAUSIBLE ns=%u",
            (unsigned)s_measured_tconv_ns);
    }

    uint8_t self_test_window[1] = {0};
    ad7606_serial_diag_state_t self_test_diag;
    ad7606_serial_diag_event_t self_test_event;
    ad7606_serial_diag_reset(&self_test_diag, self_test_window, 1U);
    (void)ad7606_serial_diag_observe(
        &self_test_diag,
        s_last_serial_observation.rx,
        &self_test_event
    );

    ESP_LOGI(
        TAG,
        "AD7606_SELF_TEST conversion=%s spi=%s payload=%s pattern=%s busy=%d",
        s_last_serial_observation.conversion_complete ? "OK" : "FAIL",
        s_last_serial_observation.spi_transaction_ok ? "OK" : "FAIL",
        s_last_serial_observation.pattern == AD7606_SERIAL_PATTERN_VARIED
            ? "PLAUSIBLE"
            : "SUSPICIOUS",
        ad7606_serial_pattern_name(
            s_last_serial_observation.pattern
        ),
        s_last_serial_observation.busy_level
    );

    ESP_LOGI(
        TAG,
        "Self-test raw=[%d,%d,%d,%d]",
        sample.raw[0],
        sample.raw[1],
        sample.raw[2],
        sample.raw[3]
    );

    ESP_LOGI(
        TAG,
        "AD7606_SERIAL_DIAG: rx=%02X %02X %02X %02X %02X %02X %02X %02X words=[%d,%d,%d,%d] class=%s busy=%d samples=0 streak=%u all_zero=%u all_ff=%u identical=%u varied=%u suspicious=%s",
        s_last_serial_observation.rx[0],
        s_last_serial_observation.rx[1],
        s_last_serial_observation.rx[2],
        s_last_serial_observation.rx[3],
        s_last_serial_observation.rx[4],
        s_last_serial_observation.rx[5],
        s_last_serial_observation.rx[6],
        s_last_serial_observation.rx[7],
        sample.raw[0],
        sample.raw[1],
        sample.raw[2],
        sample.raw[3],
        ad7606_serial_pattern_name(
            s_last_serial_observation.pattern
        ),
        s_last_serial_observation.busy_level,
        (unsigned)self_test_diag.suspicious_streak,
        (unsigned)self_test_diag.all_zero_frames,
        (unsigned)self_test_diag.all_ff_frames,
        (unsigned)self_test_diag.all_identical_frames,
        (unsigned)self_test_diag.varied_frames,
        self_test_diag.serial_suspicious ? "yes" : "no"
    );

    return ESP_OK;
}


esp_err_t ad7606_driver_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_busy_release_error_logged =
        false;
    s_measured_tconv_ns = 0U;
    s_measured_tconv_overhead_ns = 0U;
    s_measured_tconv_retries = 0U;

    esp_err_t err =
        ad7606_configure_gpio();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO configuration failed: %s",
            esp_err_to_name(err)
        );

        ad7606_driver_enter_safe_state();
        return err;
    }

    err =
        ad7606_init_spi_bus();

    if (err != ESP_OK) {
        goto fail;
    }

    s_spi_transaction = (spi_transaction_t) {
        .length = AD7606_SERIAL_RX_BYTES * 8U,
        .rx_buffer = s_spi_rx_buffer,
    };
    /* t19 max is 32 ns at VDRIVE=3.3 V; add 4 ns for board routing. */
    const int spi_limit_hz = spi_get_freq_limit(
        true,
        AD7606_SPI_INPUT_DELAY_ESTIMATE_NS
    );
    ESP_LOGI(
        TAG,
        "SPI GPIO-matrix limit=%d Hz configured=%d Hz input_delay=%d ns "
        "(datasheet t19 32 ns + 4 ns routing)",
        spi_limit_hz,
        CONFIG_AD7606_SPI_CLOCK_HZ,
        AD7606_SPI_INPUT_DELAY_ESTIMATE_NS
    );
    if (spi_limit_hz > 0 && CONFIG_AD7606_SPI_CLOCK_HZ > spi_limit_hz) {
        ESP_LOGW(
            TAG,
            "Configured AD7606 SCLK %d Hz exceeds SPI GPIO-matrix limit %d Hz",
            CONFIG_AD7606_SPI_CLOCK_HZ,
            spi_limit_hz
        );
    }

    err =
        ad7606_add_spi_device();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI device registration failed: %s",
            esp_err_to_name(err)
        );

        goto fail;
    }

    err =
        ad7606_hardware_reset();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Hardware RESET failed: %s",
            esp_err_to_name(err)
        );

        goto fail;
    }

    /*
     * Initialization is not considered successful until one
     * real conversion has completed and all 4 used words have been
     * read over the dedicated SPI3 bus.
     */
    err =
        ad7606_self_test();

    if (err != ESP_OK) {
        goto fail;
    }

    s_initialized =
        true;

    ESP_LOGI(
        TAG,
        "Initialized successfully: 4 channels, SPI3 mode=2, clock=%d Hz",
        CONFIG_AD7606_SPI_CLOCK_HZ
    );

    return ESP_OK;


fail:

    ad7606_release_spi();
    ad7606_driver_enter_safe_state();

    ESP_LOGE(
        TAG,
        "Initialization failed: %s",
        esp_err_to_name(err)
    );

    return err;
}


void ad7606_driver_deinit(void)
{
    ad7606_release_spi();
    ad7606_driver_enter_safe_state();

    s_initialized =
        false;

    ESP_LOGI(
        TAG,
        "Deinitialized"
    );
}


esp_err_t ad7606_driver_read(
    ad7606_sample_t *sample
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        ad7606_read_once(
            sample
        );
}


esp_err_t ad7606_driver_read_result(ad7606_sample_t *sample)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sample->timestamp_us = 0;
    s_last_serial_observation = (ad7606_serial_observation_t) {
        .conversion_complete = true,
        .busy_level = gpio_get_level(BOARD_AD7606_BUSY_GPIO),
        .pattern = AD7606_SERIAL_PATTERN_UNKNOWN,
    };
    return ad7606_spi_read_words(sample);
}


esp_err_t ad7606_driver_get_last_serial_observation(
    ad7606_serial_observation_t *observation
)
{
    if (observation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *observation =
        s_last_serial_observation;

    return ESP_OK;
}


esp_err_t ad7606_driver_get_measured_tconv_ns(uint32_t *tconv_ns)
{
    if (tconv_ns == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_measured_tconv_ns == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    *tconv_ns = s_measured_tconv_ns;
    return ESP_OK;
}


float ad7606_raw_to_uv(
    int16_t raw
)
{
    float microvolts = 0.0f;
    (void)ad7606_raw_to_uv_for_config(
        raw,
        CONFIG_AD7606_OVERSAMPLING_RATIO,
        (uint32_t)(AD7606_INPUT_RANGE_V * 1000.0f),
        &microvolts
    );
    return microvolts;
}


bool ad7606_raw_to_uv_for_config(
    int16_t raw,
    uint8_t oversampling_ratio,
    uint32_t full_scale_mv,
    float *microvolts
)
{
    if (
        microvolts == NULL ||
        (oversampling_ratio != 1U && oversampling_ratio != 2U &&
         oversampling_ratio != 4U && oversampling_ratio != 8U) ||
        (full_scale_mv != 5000U && full_scale_mv != 10000U)
    ) {
        return false;
    }

    /* OS changes digital filtering, not the bipolar transfer-function LSB. */
    *microvolts = (float)raw *
        ((float)full_scale_mv * 1000.0f / AD7606_CODES_PER_HALF_RANGE);
    return true;
}
