#include "ad7606_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"


static const char *TAG =
    "AD7606_DRIVER";


#define AD7606_SPI_CLOCK_HZ                    1000000

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

#define AD7606_FULL_SCALE_UV                    5000000.0f
#define AD7606_CODES_PER_HALF_RANGE             32768.0f


static spi_device_handle_t s_spi_device =
    NULL;

static bool s_spi_bus_initialized =
    false;

static bool s_initialized =
    false;

static bool s_busy_release_error_logged =
    false;

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


static esp_err_t ad7606_configure_gpio(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask =
            (1ULL << BOARD_AD7606_RESET_GPIO) |
            (1ULL << BOARD_AD7606_CONVST_GPIO) |
            (1ULL << BOARD_AD7606_CS_GPIO),

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &output_config
        );

    if (err != ESP_OK) {
        return err;
    }

    const gpio_config_t busy_config = {
        .pin_bit_mask =
            (1ULL << BOARD_AD7606_BUSY_GPIO),

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    err =
        gpio_config(
            &busy_config
        );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Keep the ADC deselected before SPI3 takes ownership of CS.
     * The hardware 10 kOhm pull-up on CS provides the same safe
     * state before the ESP32 configures this GPIO.
     */
    err =
        gpio_set_level(
            BOARD_AD7606_CS_GPIO,
            1
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        gpio_set_level(
            BOARD_AD7606_RESET_GPIO,
            0
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        gpio_set_level(
            BOARD_AD7606_CONVST_GPIO,
            0
        );

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


esp_err_t ad7606_driver_safe_gpio_init(void)
{
    return ad7606_configure_gpio();
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
            AD7606_CHANNEL_COUNT * 2,

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
            SPI_DMA_CH_AUTO
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
        "SPI3 initialized successfully: DMA=auto (SPI_DMA_CH_AUTO)"
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
            AD7606_SPI_CLOCK_HZ,

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
            AD7606_SPI_CLOCK_HZ,
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


/*
 * One complete simultaneous conversion + 8-channel SPI read.
 *
 * This internal function deliberately does NOT check
 * s_initialized, because init() itself uses it for the
 * first hardware self-test.
 */
static esp_err_t ad7606_read_once(
    ad7606_sample_t *sample
)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sample->timestamp_us =
        0;

    s_last_serial_observation =
        (ad7606_serial_observation_t) {
            .pattern = AD7606_SERIAL_PATTERN_UNKNOWN,
        };

    /*
     * Start simultaneous conversion.
     * CVA and CVB are physically tied together to GPIO42.
     */
    esp_err_t err =
        gpio_set_level(
            BOARD_AD7606_CONVST_GPIO,
            1
        );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Capture the sample timestamp immediately after the rising
     * CONVST edge, rather than after BUSY handling and SPI readout.
     */
    sample->timestamp_us =
        esp_timer_get_time();

    esp_rom_delay_us(
        AD7606_CONVST_PULSE_US
    );

    err =
        gpio_set_level(
            BOARD_AD7606_CONVST_GPIO,
            0
        );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Do not require software to catch the short BUSY-HIGH
     * interval. Wait beyond the conversion time first.
     */
    esp_rom_delay_us(
        AD7606_CONVERSION_GUARD_US
    );

    if (
        gpio_get_level(
            BOARD_AD7606_BUSY_GPIO
        ) != 0
    ) {
        err =
            ad7606_wait_busy_level(
                0,
                AD7606_BUSY_RELEASE_TIMEOUT_US
            );

        if (err != ESP_OK) {
            if (!s_busy_release_error_logged) {
                ESP_LOGE(
                    TAG,
                    "BUSY remained HIGH after conversion; level=%d",
                    gpio_get_level(
                        BOARD_AD7606_BUSY_GPIO
                    )
                );

                s_busy_release_error_logged =
                    true;
            }

            return err;
        }
    }

    esp_rom_delay_us(
        AD7606_POST_CONVERSION_DELAY_US
    );

    s_last_serial_observation.conversion_complete =
        true;
    s_last_serial_observation.busy_level =
        gpio_get_level(
            BOARD_AD7606_BUSY_GPIO
        );

    uint8_t rx[
        AD7606_CHANNEL_COUNT * 2
    ] = {0};

    spi_transaction_t transaction = {
        .length =
            sizeof(rx) * 8,

        .rx_buffer =
            rx,
    };

    err =
        spi_device_transmit(
            s_spi_device,
            &transaction
        );

    if (err != ESP_OK) {
        return err;
    }

    memcpy(
        s_last_serial_observation.rx,
        rx,
        sizeof(rx)
    );
    s_last_serial_observation.pattern =
        ad7606_serial_classify(
            rx
        );
    s_last_serial_observation.spi_transaction_ok =
        true;

    if (!ad7606_serial_decode_frame(rx, sample->raw)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
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

    ad7606_serial_diag_state_t self_test_diag;
    ad7606_serial_diag_event_t self_test_event;
    ad7606_serial_diag_reset(&self_test_diag);
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
        "Self-test raw=[%d,%d,%d,%d,%d,%d,%d,%d]",
        sample.raw[0],
        sample.raw[1],
        sample.raw[2],
        sample.raw[3],
        sample.raw[4],
        sample.raw[5],
        sample.raw[6],
        sample.raw[7]
    );

    ESP_LOGI(
        TAG,
        "AD7606_SERIAL_DIAG: rx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X words=[%d,%d,%d,%d,%d,%d,%d,%d] class=%s busy=%d samples=0 streak=%u all_zero=%u all_ff=%u identical=%u varied=%u suspicious=%s",
        s_last_serial_observation.rx[0],
        s_last_serial_observation.rx[1],
        s_last_serial_observation.rx[2],
        s_last_serial_observation.rx[3],
        s_last_serial_observation.rx[4],
        s_last_serial_observation.rx[5],
        s_last_serial_observation.rx[6],
        s_last_serial_observation.rx[7],
        s_last_serial_observation.rx[8],
        s_last_serial_observation.rx[9],
        s_last_serial_observation.rx[10],
        s_last_serial_observation.rx[11],
        s_last_serial_observation.rx[12],
        s_last_serial_observation.rx[13],
        s_last_serial_observation.rx[14],
        s_last_serial_observation.rx[15],
        sample.raw[0],
        sample.raw[1],
        sample.raw[2],
        sample.raw[3],
        sample.raw[4],
        sample.raw[5],
        sample.raw[6],
        sample.raw[7],
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

    esp_err_t err =
        ad7606_configure_gpio();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO configuration failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        ad7606_init_spi_bus();

    if (err != ESP_OK) {
        goto fail;
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
     * real conversion has completed and all 8 words have been
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
        "Initialized successfully: 8 channels, SPI3 mode=2, clock=%d Hz",
        AD7606_SPI_CLOCK_HZ
    );

    return ESP_OK;


fail:

    ad7606_release_spi();

    (void)gpio_set_level(
        BOARD_AD7606_CS_GPIO,
        1
    );

    (void)gpio_set_level(
        BOARD_AD7606_CONVST_GPIO,
        0
    );

    (void)gpio_set_level(
        BOARD_AD7606_RESET_GPIO,
        0
    );

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

    (void)gpio_set_level(
        BOARD_AD7606_CS_GPIO,
        1
    );

    (void)gpio_set_level(
        BOARD_AD7606_CONVST_GPIO,
        0
    );

    (void)gpio_set_level(
        BOARD_AD7606_RESET_GPIO,
        0
    );

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


float ad7606_raw_to_uv(
    int16_t raw
)
{
    return
        (float)raw *
        (
            AD7606_FULL_SCALE_UV /
            AD7606_CODES_PER_HALF_RANGE
        );
}
