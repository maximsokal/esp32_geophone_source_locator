#ifndef NRF_CS_TRACE_ZEPHYR_H
#define NRF_CS_TRACE_ZEPHYR_H

#include <stdint.h>

#include "nrf_cs_pair_adapter.h"

/*
 * Notes are cheap and may be made from the BLE work items; nothing is printed there. The main loop calls
 * nrf_cs_trace_zephyr_console_drain(), which prints a few queued lines per call. See nrf_cs_trace.h.
 * Without CONFIG_APP_NRF_VERBOSE_LOG every function does nothing.
 */
void nrf_cs_trace_zephyr_execute(uint32_t attempt_id, uint8_t initiator, uint8_t reflector,
    uint8_t attempt_number, uint8_t pair_index, uint32_t budget_ms);
void nrf_cs_trace_zephyr_event(const char *label, uint32_t attempt_id, int status, uint8_t phase);
void nrf_cs_trace_zephyr_timeout(uint32_t attempt_id, uint8_t phase, uint32_t budget_ms);
/* `timing` (may be NULL) adds when the outcome was known and how long the cleanup took (CS-TIME-01). */
void nrf_cs_trace_zephyr_result(uint32_t attempt_id, uint8_t outcome, int32_t distance_mm,
    uint16_t confidence, uint8_t phase, const nrf_cs_pair_adapter_timing_t *timing);
void nrf_cs_trace_zephyr_rf(uint32_t attempt_id, uint32_t signal_word, uint32_t quality_word);
/* The library's estimates in metres (NAN or negative = none): which of them is closest to the truth decides the method. */
void nrf_cs_trace_zephyr_estimates(uint32_t attempt_id, float ifft_m, float phase_slope_m, float rtt_m, float best_m,
    uint8_t tones_planned, uint8_t tones_usable, uint8_t tones_gap, uint8_t tones_lead, uint8_t tones_tail,
    uint16_t peak_floor_x10, float stock_ifft_m, const char *why);
void nrf_cs_trace_zephyr_console_drain(void);

#endif
