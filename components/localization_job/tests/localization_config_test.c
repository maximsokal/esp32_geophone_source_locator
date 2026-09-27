#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "localization_config.h"

static void test_full_settings_golden(void)
{
    cJSON *root = cJSON_Parse(
        "{\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":12,"
        "\"timing_uncertainty_us\":250,\"noise_window_samples\":80,"
        "\"sta_window_samples\":20,\"lta_window_samples\":200,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":12,"
        "\"refine_lookahead_samples\":24,\"refine_amplitude_fraction\":0.18,"
        "\"snr_signal_window_samples\":40,\"minimum_snr\":2.25,"
        "\"semblance_window_half_span_samples\":8,\"max_source_depth_m\":6.5,"
        "\"s_min_separation_samples\":120,\"s_max_separation_samples\":800,"
        "\"s_sta_window_samples\":30,\"s_lta_window_samples\":300,"
        "\"s_sta_lta_trigger_ratio\":4,\"s_minimum_snr\":3,"
        "\"propagation_model_name\":\"tap-2026\",\"time_budget_s\":30}"
    );
    assert(root != NULL);
    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
    assert(config.propagation_speed_mps == 340.0);
    assert(config.propagation_speed_uncertainty_mps == 12.0);
    assert(config.timing_uncertainty_us == 250.0);
    assert(config.pick_config.noise_window_samples == 80U);
    assert(config.pick_config.sta_window_samples == 20U);
    assert(config.pick_config.lta_window_samples == 200U);
    assert(config.pick_config.sta_lta_trigger_ratio == 3.5f);
    assert(config.pick_config.refine_lookback_samples == 12U);
    assert(config.pick_config.refine_lookahead_samples == 24U);
    assert(config.pick_config.refine_amplitude_fraction == 0.18f);
    assert(config.pick_config.snr_signal_window_samples == 40U);
    assert(config.pick_config.minimum_snr == 2.25f);
    assert(config.semblance_window_half_span_samples == 8U);
    assert(config.max_source_depth_m == 6.5);
    assert(config.s_pick_enabled);
    assert(config.s_pick_config.min_separation_samples == 120U);
    assert(config.s_pick_config.max_separation_samples == 800U);
    assert(config.s_pick_config.sta_window_samples == 30U);
    assert(config.s_pick_config.lta_window_samples == 300U);
    assert(config.s_pick_config.sta_lta_trigger_ratio == 4.0f);
    assert(config.s_pick_config.minimum_snr == 3.0f);
    assert(strcmp(config.propagation_model_name, "tap-2026") == 0);
    assert(budget_ms == 30000U);
    cJSON_Delete(root);
}

static void test_no_defaults_and_incomplete_s_rejected(void)
{
    const char *base =
        "{\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":12,"
        "\"timing_uncertainty_us\":250,\"noise_window_samples\":80,"
        "\"sta_window_samples\":20,\"lta_window_samples\":200,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":12,"
        "\"refine_lookahead_samples\":24,\"refine_amplitude_fraction\":0.18,"
        "\"snr_signal_window_samples\":40,\"minimum_snr\":2.25,"
        "\"semblance_window_half_span_samples\":8";
    char json[768];
    (void)snprintf(json, sizeof(json), "%s}", base);
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    localization_calculate_config_t config;
    uint32_t budget_ms = 99U;
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
    assert(config.max_source_depth_m == 0.0);
    assert(!config.s_pick_enabled);
    assert(config.propagation_model_name[0] == '\0');
    assert(budget_ms == 0U);
    cJSON_Delete(root);

    (void)snprintf(json, sizeof(json), "%s,\"s_sta_window_samples\":30}", base);
    root = cJSON_Parse(json);
    assert(root != NULL);
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_ERR_INVALID_ARG);
    cJSON_Delete(root);
}

int main(void)
{
    test_full_settings_golden();
    test_no_defaults_and_incomplete_s_rejected();
    return 0;
}
