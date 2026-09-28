#include "ad7606_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

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
#define AD7606_BUSY_RELEASE_TIMEOUT_US          100

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
    uint32_t *edge_cycles
)
{
    for (;;) {
        const uint32_t now_cycles = (uint32_t)esp_cpu_get_cycle_count();
        if (gpio_get_level(BOARD_AD7606_BUSY_GPIO) == expected_level) {
            *edge_cycles = now_cycles;
            return ESP_OK;
        }
        if ((uint32_t)(now_cycles - timeout_start_cycles) >= timeout_cycles) {
            return ESP_ERR_TIMEOUT;
        }
    }
}


static esp_err_t ad7606_measure_busy_tconv(void)
{
    const int cpu_hz = esp_clk_cpu_freq();
    if (cpu_hz <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ad7606_wait_busy_level(0, AD7606_BUSY_RELEASE_TIMEOUT_US);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(BOARD_AD7606_CONVST_GPIO, 1);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(AD7606_CONVST_PULSE_US);
    err = gpio_set_level(BOARD_AD7606_CONVST_GPIO, 0);
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t timeout_cycles_64 =
        (uint64_t)(unsigned)cpu_hz * AD7606_BUSY_RELEASE_TIMEOUT_US / 1000000U;
    if (timeout_cycles_64 == 0U || timeout_cycles_64 > UINT32_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t timeout_start_cycles = (uint32_t)esp_cpu_get_cycle_count();
    const uint32_t timeout_cycles = (uint32_t)timeout_cycles_64;
    uint32_t busy_rising_cycles = 0U;
    uint32_t busy_falling_cycles = 0U;

    err = ad7606_wait_busy_edge_cycles(
        1, timeout_start_cycles, timeout_cycles, &busy_rising_cycles);
    if (err == ESP_OK) {
        err = ad7606_wait_busy_edge_cycles(
            0, timeout_start_cycles, timeout_cycles, &busy_falling_cycles);
    }
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t width_cycles = busy_falling_cycles - busy_rising_cycles;
    const uint64_t width_ns =
        ((uint64_t)width_cycles * UINT64_C(1000000000) + (uint32_t)cpu_hz / 2U) /
        (uint32_t)cpu_hz;
    if (width_cycles == 0U || width_ns > UINT32_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_measured_tconv_ns = (uint32_t)width_ns;
    return ESP_OK;
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
        err = ad7606_wait_busy_level(0, AD7606_BUSY_RELEASE_TIMEOUT_US);
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
        return err;
    }
    ESP_LOGI(TAG, "AD7606_TCONV_MEASURED: %u ns",
        (unsigned)s_measured_tconv_ns);

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
