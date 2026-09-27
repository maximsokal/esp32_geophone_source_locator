#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LOCALIZATION_CALCULATE_STUB_MODEL_NAME_SIZE 32U

typedef struct
{
    size_t noise_window_samples;
    size_t sta_window_samples;
    size_t lta_window_samples;
    float sta_lta_trigger_ratio;
    size_t refine_lookback_samples;
    size_t refine_lookahead_samples;
    float refine_amplitude_fraction;
    size_t snr_signal_window_samples;
    float minimum_snr;

} geophone_channel_pick_config_t;

typedef struct
{
    size_t min_separation_samples;
    size_t max_separation_samples;
    size_t sta_window_samples;
    size_t lta_window_samples;
    float sta_lta_trigger_ratio;
    float minimum_snr;

} geophone_secondary_pick_config_t;

typedef struct
{
    double propagation_speed_mps;
    double propagation_speed_uncertainty_mps;
    char propagation_model_name[LOCALIZATION_CALCULATE_STUB_MODEL_NAME_SIZE];
    char material[24];
    double timing_uncertainty_us;
    geophone_channel_pick_config_t pick_config;
    size_t semblance_window_half_span_samples;
    double max_source_depth_m;
    bool s_pick_enabled;
    geophone_secondary_pick_config_t s_pick_config;

} localization_calculate_config_t;

typedef struct
{
    size_t participating_channel_count;
    size_t methods_attempted;
    size_t methods_solved;
    size_t methods_persisted;

} localization_calculate_summary_t;

esp_err_t localization_calculate_run(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary_out
);
