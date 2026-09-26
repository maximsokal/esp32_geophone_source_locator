#include "nrf_ema_filter.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

bool nrf_ema_filter_init(
    nrf_ema_filter_t *filter,
    uint32_t tau_ms,
    double min_value,
    double max_value)
{
    if (filter == NULL || tau_ms == 0U || !isfinite(min_value) ||
        !isfinite(max_value) || min_value > max_value) {
        return false;
    }
    *filter = (nrf_ema_filter_t){
        .min_value = min_value,
        .max_value = max_value,
        .tau_ms = tau_ms,
    };
    return true;
}

void nrf_ema_filter_reset(nrf_ema_filter_t *filter)
{
    if (filter == NULL) {
        return;
    }
    filter->value = 0.0;
    filter->timestamp_ms = 0U;
    filter->initialized = false;
}

nrf_ema_filter_result_t nrf_ema_filter_update(
    nrf_ema_filter_t *filter,
    double sample,
    uint64_t timestamp_ms,
    double *filtered_value)
{
    uint64_t dt_ms;

    if (filter == NULL || filtered_value == NULL || filter->tau_ms == 0U) {
        return NRF_EMA_FILTER_INVALID_SAMPLE;
    }
    if (!isfinite(sample) || sample < filter->min_value ||
        sample > filter->max_value) {
        return NRF_EMA_FILTER_INVALID_SAMPLE;
    }
    if (!filter->initialized) {
        filter->value = sample;
        filter->timestamp_ms = timestamp_ms;
        filter->initialized = true;
        *filtered_value = sample;
        return NRF_EMA_FILTER_UPDATED;
    }
    if (timestamp_ms <= filter->timestamp_ms) {
        return NRF_EMA_FILTER_INVALID_TIME;
    }

    dt_ms = timestamp_ms - filter->timestamp_ms;
    if (dt_ms > (uint64_t)filter->tau_ms * 5U) {
        filter->value = sample;
    } else {
        const double alpha = (double)dt_ms /
            ((double)filter->tau_ms + (double)dt_ms);
        filter->value += alpha * (sample - filter->value);
    }
    filter->timestamp_ms = timestamp_ms;
    *filtered_value = filter->value;
    return NRF_EMA_FILTER_UPDATED;
}
