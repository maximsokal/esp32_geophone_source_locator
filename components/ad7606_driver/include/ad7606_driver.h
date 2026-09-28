#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ad7606_serial.h"


#define AD7606_CHANNEL_COUNT          4U
#define AD7606_SPI_MODE                2U
#define AD7606_INPUT_RANGE_V           5.0f

#if AD7606_CHANNEL_COUNT != AD7606_SERIAL_WORD_COUNT
#error "AD7606 channel count must match the serial frame word count"
#endif


typedef struct
{
    /* The hardware-timed acquisition worker assigns this from its sample index. */
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
 * MAX-242: release RESET, CONVST, CS and SCLK to Hi-Z inputs without
 * internal pulls (CS stays deselected through its external pull-up to the
 * AD7606 VIO). Used by deinit, by every init failure path and when the
 * AD7606 +5 V rail is lost; safe to call at any time.
 */
void ad7606_driver_enter_safe_state(void);

esp_err_t ad7606_driver_read(
    ad7606_sample_t *sample
);

/* Read the latest four DOUTA words without starting a conversion. */
esp_err_t ad7606_driver_read_result(
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

bool ad7606_raw_to_uv_for_config(
    int16_t raw,
    uint8_t oversampling_ratio,
    uint32_t full_scale_mv,
    float *microvolts
);

/* Used when the acquisition owns BUSY's GPIO ISR and PCNT observes pulses. */
void ad7606_driver_presence_use_external_busy_isr(bool enabled);
void ad7606_driver_presence_note_busy_pulse(void);
