#include "ad7606_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"
#include "spi_bus.h"

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
 * 100 us gives generous margin for Stage 11A polling.
 */
#define AD7606_BUSY_RELEASE_TIMEOUT_US          100

/*
 * Boot-only idle wait.
 *
 * This is NOT a RESET validation rule.
 * We simply must not start a conversion while BUSY is HIGH,
 * because a CONVST edge while BUSY is active may be ignored.
 */
#define AD7606_STARTUP_IDLE_TIMEOUT_US          50000

#define AD7606_FULL_SCALE_UV                    5000000.0f
#define AD7606_CODES_PER_HALF_RANGE             32768.0f


static spi_device_handle_t s_spi_device =
    NULL;

static bool s_initialized =
    false;

static bool s_busy_release_error_logged =
    false;


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
            (1ULL << BOARD_AD7606_CONVST_GPIO),

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
        "GPIO configured: RESET=%d CONVST=%d BUSY=%d",
        BOARD_AD7606_RESET_GPIO,
        BOARD_AD7606_CONVST_GPIO,
        BOARD_AD7606_BUSY_GPIO
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
            app_spi_bus_get_host(),
            &config,
            &s_spi_device
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "SPI registered: SPI2 mode=2 clock=%d Hz CS=%d",
            AD7606_SPI_CLOCK_HZ,
            BOARD_AD7606_CS_GPIO
        );
    }

    return err;
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
 * One complete conversion + 8-channel SPI read.
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

    /*
     * Start simultaneous conversion.
     * CVA and CVB are physically tied together to GPIO9.
     */
    esp_err_t err =
        gpio_set_level(
            BOARD_AD7606_CONVST_GPIO,
            1
        );

    if (err != ESP_OK) {
        return err;
    }

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
     * interval.
     *
     * Wait beyond the conversion time first.
     */
    esp_rom_delay_us(
        AD7606_CONVERSION_GUARD_US
    );

    /*
     * If conversion is still active, wait for its completion.
     */
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

    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        const uint16_t word =
            (
                (uint16_t)rx[
                    channel * 2
                ] << 8
            ) |
            (uint16_t)rx[
                channel * 2 + 1
            ];

        sample->raw[channel] =
            (int16_t)word;
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
     *
     * Therefore the authoritative initialization test is the
     * conversion itself, not BUSY state before the first CONVST.
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

    ESP_LOGI(
        TAG,
        "Self-test conversion + SPI read OK"
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
        ad7606_add_spi_device();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI device registration failed: %s",
            esp_err_to_name(err)
        );

        return err;
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
     * read over SPI.
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
        "Initialized successfully: 8 channels, SPI2 mode=2, clock=%d Hz",
        AD7606_SPI_CLOCK_HZ
    );

    return ESP_OK;


fail:

    if (s_spi_device != NULL) {
        (void)spi_bus_remove_device(
            s_spi_device
        );

        s_spi_device =
            NULL;
    }

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
    if (s_spi_device != NULL) {
        (void)spi_bus_remove_device(
            s_spi_device
        );

        s_spi_device =
            NULL;
    }

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