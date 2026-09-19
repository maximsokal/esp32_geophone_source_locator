#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ad7606_serial.h"


#define AD7606_CHANNEL_COUNT          8
#define AD7606_BUS_CLOCK_HZ     1000000U
#define AD7606_SPI_MODE                2U
#define AD7606_INPUT_RANGE_V           5.0f
#define AD7606_OVERSAMPLING_RATIO      1U

#if AD7606_CHANNEL_COUNT != AD7606_SERIAL_WORD_COUNT
#error "AD7606 channel count must match the serial frame word count"
#endif


typedef struct
{
    /*
     * Timestamp captured immediately after the CONVST rising edge
     * that starts the simultaneous conversion of all 8 channels.
     */
    int64_t timestamp_us;

    int16_t raw[AD7606_CHANNEL_COUNT];

} ad7606_sample_t;


typedef struct
{
    bool conversion_complete;
    bool spi_transaction_ok;
    int busy_level;

    ad7606_serial_pattern_t pattern;
    uint8_t rx[AD7606_SERIAL_RX_BYTES];

} ad7606_serial_observation_t;


esp_err_t ad7606_driver_init(void);

void ad7606_driver_deinit(void);

/*
 * Puts the AD7606 GPIOs (RESET, CONVST, CS, BUSY) into their safe idle
 * state without touching SPI3: CS high (deselected), RESET and CONVST
 * low (inactive), BUSY as input. The chip is physically powered from
 * boot onward regardless of whether the driver has been fully
 * initialized, so these pins must not be left floating between power-on
 * and the first call to ad7606_driver_init() (which also calls this
 * internally, so calling it again there is redundant but harmless).
 */
esp_err_t ad7606_driver_safe_gpio_init(void);

esp_err_t ad7606_driver_read(
    ad7606_sample_t *sample
);


/* Snapshot the most recent successful serial transaction for diagnostics. */
esp_err_t ad7606_driver_get_last_serial_observation(
    ad7606_serial_observation_t *observation
);

/*
 * Physical presence is established from real BUSY LOW-HIGH-LOW conversion
 * pulses, not from SPI receiving a plausible constant byte pattern. This is
 * intentionally separate from transport initialization because an open MISO
 * line can still complete an SPI transaction.
 */
esp_err_t ad7606_driver_presence_monitor_start(void);
void ad7606_driver_presence_monitor_stop(void);
bool ad7606_driver_presence_is_healthy(void);

float ad7606_raw_to_uv(
    int16_t raw
);
