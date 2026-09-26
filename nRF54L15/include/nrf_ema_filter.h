#ifndef NRF_EMA_FILTER_H
#define NRF_EMA_FILTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_EMA_FILTER_UPDATED = 0,
    NRF_EMA_FILTER_INVALID_SAMPLE,
    NRF_EMA_FILTER_INVALID_TIME,
} nrf_ema_filter_result_t;

typedef struct {
    double value;
    double min_value;
    double max_value;
    uint64_t timestamp_ms;
    uint32_t tau_ms;
    bool initialized;
} nrf_ema_filter_t;

bool nrf_ema_filter_init(
    nrf_ema_filter_t *filter,
    uint32_t tau_ms,
    double min_value,
    double max_value);

void nrf_ema_filter_reset(nrf_ema_filter_t *filter);

nrf_ema_filter_result_t nrf_ema_filter_update(
    nrf_ema_filter_t *filter,
    double sample,
    uint64_t timestamp_ms,
    double *filtered_value);

#ifdef __cplusplus
}
#endif

#endif
