#include "localization_config.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if CONFIG_SEISMIC_SOURCE_TEST
#include "seismic_test_generator.h"
#define LOCALIZATION_CONFIG_SAMPLE_RATE_HZ SEISMIC_TEST_GENERATOR_SAMPLE_RATE_HZ
#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
#define LOCALIZATION_CONFIG_SAMPLE_RATE_HZ CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ
#elif CONFIG_SEISMIC_SOURCE_AD7606
#define LOCALIZATION_CONFIG_SAMPLE_RATE_HZ CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
#else
#error "No seismic acquisition source selected"
#endif
#else
/* Native host defaults exercise the production AD7606 500 Hz profile. */
#define LOCALIZATION_CONFIG_SAMPLE_RATE_HZ 500U
#endif

#include "localization_job.h"
#include "localization_material_presets.h"

#ifndef ESP_PLATFORM
/* Existing host fragments compile localization_config.c as a single source. */
#include "localization_material_presets.c"
#endif

static size_t samples_for_ms(uint32_t sample_rate_hz, uint32_t milliseconds)
{
    return (size_t)(((uint64_t)sample_rate_hz * milliseconds + 500U) / 1000U);
}

esp_err_t localization_config_standard_processing(
    localization_calculate_config_t *config_out,
    uint32_t *budget_ms_out
)
{
    if (config_out == NULL || budget_ms_out == NULL || LOCALIZATION_CONFIG_SAMPLE_RATE_HZ == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config_out, 0, sizeof(*config_out));
    config_out->timing_uncertainty_us =
        1000000.0 / (double)LOCALIZATION_CONFIG_SAMPLE_RATE_HZ;
    config_out->pick_config.noise_window_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 100U);
    config_out->pick_config.sta_window_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 20U);
    config_out->pick_config.lta_window_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 200U);
    config_out->pick_config.sta_lta_trigger_ratio = 3.0f;
    config_out->pick_config.refine_lookback_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 20U);
    config_out->pick_config.refine_lookahead_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 20U);
    config_out->pick_config.refine_amplitude_fraction = 0.5f;
    config_out->pick_config.snr_signal_window_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 40U);
    config_out->pick_config.minimum_snr = 3.0f;
    config_out->semblance_window_half_span_samples = samples_for_ms(LOCALIZATION_CONFIG_SAMPLE_RATE_HZ, 20U);
    config_out->s_pick_enabled = false;
    *budget_ms_out = LOCALIZATION_JOB_DEFAULT_BUDGET_MS;
    return ESP_OK;
}

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
    if (number_item(root, key, &raw) == NULL || raw < 0.0 || raw >= (double)SIZE_MAX || floor(raw) != raw) {
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
    const cJSON *material = cJSON_GetObjectItemCaseSensitive(root, "material");
    const char *material_id = "custom";
    const localization_material_preset_t *preset = NULL;
    if (material != NULL) {
        if (!cJSON_IsString(material) || material->valuestring == NULL) return ESP_ERR_INVALID_ARG;
        material_id = material->valuestring;
        if (strcmp(material_id, "custom") != 0) {
            preset = localization_material_preset_lookup(material_id);
            if (preset == NULL) return ESP_ERR_INVALID_ARG;
        }
    }
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
        !(speed > 50.0 && speed < 8000.0) ||
        !(speed_uncertainty > 0.0 && speed_uncertainty < speed) ||
        !(timing_uncertainty > 0.0) ||
        (preset != NULL &&
            (speed != (double)preset->propagation_speed_mps ||
             speed_uncertainty != (double)preset->propagation_speed_uncertainty_mps)) ||
        config.pick_config.noise_window_samples == 0U ||
        config.pick_config.sta_window_samples == 0U ||
        config.pick_config.lta_window_samples <= config.pick_config.sta_window_samples ||
        !(trigger_ratio > 0.0 && trigger_ratio <= FLT_MAX) ||
        trigger_ratio >= (double)config.pick_config.lta_window_samples /
            (double)config.pick_config.sta_window_samples ||
        config.pick_config.refine_lookback_samples == 0U ||
        config.pick_config.refine_lookahead_samples == 0U ||
        !(refine_fraction > 0.0 && refine_fraction <= 1.0) ||
        config.pick_config.snr_signal_window_samples == 0U ||
        !(minimum_snr >= 0.0 && minimum_snr <= FLT_MAX) ||
        config.semblance_window_half_span_samples == 0U
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

    const cJSON *s_enabled = cJSON_GetObjectItemCaseSensitive(root, "s_pick_enabled");
    if (s_enabled != NULL && !cJSON_IsBool(s_enabled)) return ESP_ERR_INVALID_ARG;
    const bool s_fields_present =
        cJSON_GetObjectItemCaseSensitive(root, "s_min_separation_samples") != NULL ||
        cJSON_GetObjectItemCaseSensitive(root, "s_max_separation_samples") != NULL ||
        cJSON_GetObjectItemCaseSensitive(root, "s_sta_window_samples") != NULL ||
        cJSON_GetObjectItemCaseSensitive(root, "s_lta_window_samples") != NULL ||
        cJSON_GetObjectItemCaseSensitive(root, "s_sta_lta_trigger_ratio") != NULL ||
        cJSON_GetObjectItemCaseSensitive(root, "s_minimum_snr") != NULL;
    const bool s_enabled_value = s_enabled == NULL ? s_fields_present : cJSON_IsTrue(s_enabled);
    if ((s_enabled_value && !s_fields_present) || (!s_enabled_value && s_fields_present)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_enabled_value) {
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
    if (model_name != NULL &&
        (!cJSON_IsString(model_name) || model_name->valuestring == NULL ||
         strnlen(model_name->valuestring, sizeof(config.propagation_model_name)) >=
            sizeof(config.propagation_model_name))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (model_name != NULL) {
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

static bool add_processing_fields(cJSON *root, const localization_calculate_config_t *config)
{
    const geophone_channel_pick_config_t *pick = &config->pick_config;
    if (cJSON_AddNumberToObject(root, "timing_uncertainty_us", config->timing_uncertainty_us) == NULL ||
        cJSON_AddNumberToObject(root, "noise_window_samples", (double)pick->noise_window_samples) == NULL ||
        cJSON_AddNumberToObject(root, "sta_window_samples", (double)pick->sta_window_samples) == NULL ||
        cJSON_AddNumberToObject(root, "lta_window_samples", (double)pick->lta_window_samples) == NULL ||
        cJSON_AddNumberToObject(root, "sta_lta_trigger_ratio", pick->sta_lta_trigger_ratio) == NULL ||
        cJSON_AddNumberToObject(root, "refine_lookback_samples", (double)pick->refine_lookback_samples) == NULL ||
        cJSON_AddNumberToObject(root, "refine_lookahead_samples", (double)pick->refine_lookahead_samples) == NULL ||
        cJSON_AddNumberToObject(root, "refine_amplitude_fraction", pick->refine_amplitude_fraction) == NULL ||
        cJSON_AddNumberToObject(root, "snr_signal_window_samples", (double)pick->snr_signal_window_samples) == NULL ||
        cJSON_AddNumberToObject(root, "minimum_snr", pick->minimum_snr) == NULL ||
        cJSON_AddNumberToObject(root, "semblance_window_half_span_samples",
            (double)config->semblance_window_half_span_samples) == NULL) {
        return false;
    }
    if (config->s_pick_enabled) {
        const geophone_secondary_pick_config_t *s_pick = &config->s_pick_config;
        return cJSON_AddBoolToObject(root, "s_pick_enabled", true) != NULL &&
            cJSON_AddNumberToObject(root, "s_min_separation_samples", (double)s_pick->min_separation_samples) != NULL &&
            cJSON_AddNumberToObject(root, "s_max_separation_samples", (double)s_pick->max_separation_samples) != NULL &&
            cJSON_AddNumberToObject(root, "s_sta_window_samples", (double)s_pick->sta_window_samples) != NULL &&
            cJSON_AddNumberToObject(root, "s_lta_window_samples", (double)s_pick->lta_window_samples) != NULL &&
            cJSON_AddNumberToObject(root, "s_sta_lta_trigger_ratio", s_pick->sta_lta_trigger_ratio) != NULL &&
            cJSON_AddNumberToObject(root, "s_minimum_snr", s_pick->minimum_snr) != NULL;
    }
    return cJSON_AddBoolToObject(root, "s_pick_enabled", false) != NULL;
}

esp_err_t localization_config_to_json(
    const localization_calculate_config_t *config,
    const char *material_id,
    uint32_t budget_ms,
    char **json_out
)
{
    if (config == NULL || material_id == NULL || json_out == NULL) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;
    const localization_material_preset_t *preset = localization_material_preset_lookup(material_id);
    if (strcmp(material_id, "custom") != 0 && preset == NULL) return ESP_ERR_INVALID_ARG;
    if (preset != NULL && (config->propagation_speed_mps != (double)preset->propagation_speed_mps ||
        config->propagation_speed_uncertainty_mps != (double)preset->propagation_speed_uncertainty_mps)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (budget_ms == 0U) budget_ms = LOCALIZATION_JOB_DEFAULT_BUDGET_MS;
    if (budget_ms < LOCALIZATION_JOB_MIN_BUDGET_MS || budget_ms > LOCALIZATION_JOB_MAX_BUDGET_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    bool ok = cJSON_AddStringToObject(root, "material", material_id) != NULL &&
        cJSON_AddNumberToObject(root, "propagation_speed_mps", config->propagation_speed_mps) != NULL &&
        cJSON_AddNumberToObject(root, "propagation_speed_uncertainty_mps",
            config->propagation_speed_uncertainty_mps) != NULL &&
        add_processing_fields(root, config) &&
        cJSON_AddNumberToObject(root, "time_budget_s", (double)budget_ms / 1000.0) != NULL;
    if (ok && config->propagation_model_name[0] != '\0') {
        ok = cJSON_AddStringToObject(root, "propagation_model_name", config->propagation_model_name) != NULL;
    }
    if (ok && config->max_source_depth_m > 0.0) {
        ok = cJSON_AddNumberToObject(root, "max_source_depth_m", config->max_source_depth_m) != NULL;
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;
    *json_out = json;
    return ESP_OK;
}

esp_err_t localization_config_standard_processing_to_json(char **json_out)
{
    if (json_out == NULL) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;
    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    const esp_err_t err = localization_config_standard_processing(&config, &budget_ms);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    const bool ok = add_processing_fields(root, &config) &&
        cJSON_AddNumberToObject(root, "time_budget_s", (double)budget_ms / 1000.0) != NULL;
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;
    *json_out = json;
    return ESP_OK;
}
