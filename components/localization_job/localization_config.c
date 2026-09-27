#include "localization_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "localization_job.h"

static const cJSON *number_item(const cJSON *root, const char *key, double *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return NULL;
    *value = item->valuedouble;
    return item;
}

static bool size_item(const cJSON *root, const char *key, size_t *value)
{
    double raw = 0.0;
    if (number_item(root, key, &raw) == NULL || raw < 0.0 || floor(raw) != raw) {
        return false;
    }
    *value = (size_t)raw;
    return true;
}

esp_err_t localization_config_from_json(
    const cJSON *root,
    localization_calculate_config_t *config_out,
    uint32_t *budget_ms_out
)
{
    if (root == NULL || config_out == NULL || budget_ms_out == NULL || !cJSON_IsObject(root)) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_calculate_config_t config = {0};
    double speed = 0.0;
    double speed_uncertainty = 0.0;
    double timing_uncertainty = 0.0;
    double trigger_ratio = 0.0;
    double refine_fraction = 0.0;
    double minimum_snr = 0.0;
    if (
        number_item(root, "propagation_speed_mps", &speed) == NULL ||
        number_item(root, "propagation_speed_uncertainty_mps", &speed_uncertainty) == NULL ||
        number_item(root, "timing_uncertainty_us", &timing_uncertainty) == NULL ||
        !size_item(root, "noise_window_samples", &config.pick_config.noise_window_samples) ||
        !size_item(root, "sta_window_samples", &config.pick_config.sta_window_samples) ||
        !size_item(root, "lta_window_samples", &config.pick_config.lta_window_samples) ||
        number_item(root, "sta_lta_trigger_ratio", &trigger_ratio) == NULL ||
        !size_item(root, "refine_lookback_samples", &config.pick_config.refine_lookback_samples) ||
        !size_item(root, "refine_lookahead_samples", &config.pick_config.refine_lookahead_samples) ||
        number_item(root, "refine_amplitude_fraction", &refine_fraction) == NULL ||
        !size_item(root, "snr_signal_window_samples", &config.pick_config.snr_signal_window_samples) ||
        number_item(root, "minimum_snr", &minimum_snr) == NULL ||
        !size_item(root, "semblance_window_half_span_samples", &config.semblance_window_half_span_samples)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    config.propagation_speed_mps = speed;
    config.propagation_speed_uncertainty_mps = speed_uncertainty;
    config.timing_uncertainty_us = timing_uncertainty;
    config.pick_config.sta_lta_trigger_ratio = (float)trigger_ratio;
    config.pick_config.refine_amplitude_fraction = (float)refine_fraction;
    config.pick_config.minimum_snr = (float)minimum_snr;
    if (
        config.pick_config.sta_window_samples == 0U ||
        config.pick_config.lta_window_samples <= config.pick_config.sta_window_samples ||
        trigger_ratio >= (double)config.pick_config.lta_window_samples /
            (double)config.pick_config.sta_window_samples
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *max_depth = cJSON_GetObjectItemCaseSensitive(root, "max_source_depth_m");
    if (max_depth != NULL) {
        if (!cJSON_IsNumber(max_depth) || !isfinite(max_depth->valuedouble) || max_depth->valuedouble <= 0.0) {
            return ESP_ERR_INVALID_ARG;
        }
        config.max_source_depth_m = max_depth->valuedouble;
    }

    if (cJSON_GetObjectItemCaseSensitive(root, "s_sta_window_samples") != NULL) {
        double s_trigger_ratio = 0.0;
        double s_minimum_snr = 0.0;
        geophone_secondary_pick_config_t *s_pick = &config.s_pick_config;
        if (
            !size_item(root, "s_min_separation_samples", &s_pick->min_separation_samples) ||
            !size_item(root, "s_max_separation_samples", &s_pick->max_separation_samples) ||
            !size_item(root, "s_sta_window_samples", &s_pick->sta_window_samples) ||
            !size_item(root, "s_lta_window_samples", &s_pick->lta_window_samples) ||
            number_item(root, "s_sta_lta_trigger_ratio", &s_trigger_ratio) == NULL ||
            number_item(root, "s_minimum_snr", &s_minimum_snr) == NULL ||
            s_pick->sta_window_samples == 0U ||
            s_pick->lta_window_samples <= s_pick->sta_window_samples ||
            s_pick->max_separation_samples <= s_pick->min_separation_samples ||
            !(s_trigger_ratio > 0.0) ||
            s_trigger_ratio >= (double)s_pick->lta_window_samples / (double)s_pick->sta_window_samples ||
            s_minimum_snr < 0.0
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        s_pick->sta_lta_trigger_ratio = (float)s_trigger_ratio;
        s_pick->minimum_snr = (float)s_minimum_snr;
        config.s_pick_enabled = true;
    }

    const cJSON *model_name = cJSON_GetObjectItemCaseSensitive(root, "propagation_model_name");
    if (cJSON_IsString(model_name) && model_name->valuestring != NULL) {
        (void)snprintf(config.propagation_model_name, sizeof(config.propagation_model_name), "%s", model_name->valuestring);
    }

    uint32_t budget_ms = 0U;
    const cJSON *budget = cJSON_GetObjectItemCaseSensitive(root, "time_budget_s");
    if (budget != NULL) {
        if (
            !cJSON_IsNumber(budget) || !isfinite(budget->valuedouble) ||
            budget->valuedouble * 1000.0 < (double)LOCALIZATION_JOB_MIN_BUDGET_MS ||
            budget->valuedouble * 1000.0 > (double)LOCALIZATION_JOB_MAX_BUDGET_MS
        ) {
            return ESP_ERR_INVALID_ARG;
        }
        budget_ms = (uint32_t)(budget->valuedouble * 1000.0);
    }

    *config_out = config;
    *budget_ms_out = budget_ms;
    return ESP_OK;
}
