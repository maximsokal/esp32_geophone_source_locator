#ifndef AD7606_DIAG_ACCUM_H
#define AD7606_DIAG_ACCUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ad7606_driver.h"

/* Kconfig and ad7606_acquisition_timing.c support rates through 40 kHz. */
#define AD7606_DIAG_MAX_WINDOW_SAMPLES 40000U

typedef struct
{
    uint32_t count;
    int16_t min_raw;
    int16_t max_raw;
    int64_t sum_raw;
    uint64_t sum_sq_raw;
    uint32_t saturation_count;

} ad7606_diag_channel_t;

typedef struct
{
    double mean_raw;
    double rms_raw;
    double p2p_raw;
    double peak_raw;

} ad7606_diag_metrics_t;

#if defined(AD7606_ACQUISITION_HOST_TEST)
typedef struct
{
    uint64_t status_critical_entries;
    uint64_t sample_status_critical_entries;
} ad7606_acquisition_test_counters_t;

void ad7606_acquisition_test_counters_reset(void);
void ad7606_acquisition_test_counters_get(
    ad7606_acquisition_test_counters_t *out
);
void ad7606_acquisition_test_status_delay_set(uint32_t delay_us);
#endif

static inline void ad7606_diag_channel_record(
    ad7606_diag_channel_t *diag,
    int16_t raw
)
{
    if (raw < diag->min_raw) {
        diag->min_raw = raw;
    }

    if (raw > diag->max_raw) {
        diag->max_raw = raw;
    }

    const int32_t raw_wide = (int32_t)raw;
    diag->count++;
    diag->sum_raw += raw_wide;
    diag->sum_sq_raw += (uint32_t)(raw_wide * raw_wide);

    if (raw == INT16_MIN || raw == INT16_MAX) {
        diag->saturation_count++;
    }
}

#if defined(AD7606_ACQUISITION_HOST_TEST)
typedef enum
{
    AD7606_ACQUISITION_TEST_LOG_DIAG_CHANNEL,
    AD7606_ACQUISITION_TEST_LOG_SERIAL_DIAG,
    AD7606_ACQUISITION_TEST_LOG_SERIAL_COALESCED,
    AD7606_ACQUISITION_TEST_LOG_SERIAL_SUPPRESSED,
    AD7606_ACQUISITION_TEST_LOG_OS_RESOLUTION,
    AD7606_ACQUISITION_TEST_LOG_RATE,

} ad7606_acquisition_test_log_event_t;

typedef bool (*ad7606_acquisition_test_log_sink_t)(
    ad7606_acquisition_test_log_event_t event,
    uint64_t sample_index,
    size_t channel,
    uint32_t count,
    const char *state
);

void ad7606_acquisition_test_diag_reset(void);
void ad7606_acquisition_test_diag_record(const ad7606_sample_t *sample);
void ad7606_acquisition_test_diag_snapshot(ad7606_diag_channel_t *out);
const char *ad7606_acquisition_test_diag_state(size_t channel);
bool ad7606_acquisition_test_diag_metrics(
    size_t channel,
    ad7606_diag_metrics_t *out
);
void ad7606_acquisition_test_diag_finish_sample(uint64_t sample_index);
void ad7606_acquisition_test_rate_window_set(uint32_t cycles);
void ad7606_acquisition_test_rate_flush(void);
void ad7606_acquisition_test_log_sink_set(
    ad7606_acquisition_test_log_sink_t sink
);
bool ad7606_acquisition_test_serial_diag_pending(void);
uint64_t ad7606_acquisition_test_serial_diag_last_log_sample(void);
uint32_t ad7606_acquisition_test_heap_caps_allocations(void);
#endif

#endif
