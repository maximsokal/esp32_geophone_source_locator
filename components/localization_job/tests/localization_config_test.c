#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "localization_config.h"
#include "localization_job.h"
#include "localization_material_presets.h"

static void test_material_preset_values(void)
{
    static const localization_material_preset_t expected[] = {
        { "concrete", 4000U, 500U },
        { "reinforced_concrete", 4200U, 500U },
        { "brick_masonry", 2200U, 700U },
        { "rubble", 400U, 250U },
        { "steel", 5200U, 700U },
        { "soil_dry", 500U, 300U },
        { "soil_saturated", 1500U, 500U },
    };
    assert(localization_material_preset_count() == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0U; i < localization_material_preset_count(); ++i) {
        const localization_material_preset_t *actual = localization_material_preset_at(i);
        assert(actual != NULL);
        assert(strcmp(actual->id, expected[i].id) == 0);
        assert(actual->propagation_speed_mps == expected[i].propagation_speed_mps);
        assert(actual->propagation_speed_uncertainty_mps == expected[i].propagation_speed_uncertainty_mps);
        assert(localization_material_preset_lookup(expected[i].id) == actual);
    }
    assert(localization_material_preset_at(localization_material_preset_count()) == NULL);
    assert(localization_material_preset_lookup("custom") == NULL);
    assert(localization_material_preset_lookup("not-a-material") == NULL);
}

static void test_standard_processing_is_valid_and_round_trips(void)
{
    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    assert(localization_config_standard_processing(&config, &budget_ms) == ESP_OK);
    assert(config.timing_uncertainty_us == 2000.0);
    assert(config.pick_config.noise_window_samples == 50U);
    assert(config.pick_config.sta_window_samples == 10U);
    assert(config.pick_config.lta_window_samples == 100U);
    assert(config.pick_config.sta_lta_trigger_ratio == 3.0f);
    assert(config.pick_config.refine_lookback_samples == 10U);
    assert(config.pick_config.refine_lookahead_samples == 10U);
    assert(config.pick_config.refine_amplitude_fraction == 0.5f);
    assert(config.pick_config.snr_signal_window_samples == 20U);
    assert(config.pick_config.minimum_snr == 3.0f);
    assert(config.semblance_window_half_span_samples == 10U);
    assert(!config.s_pick_enabled);
    assert(budget_ms == LOCALIZATION_JOB_DEFAULT_BUDGET_MS);

    config.propagation_speed_mps = 4000.0;
    config.propagation_speed_uncertainty_mps = 500.0;
    char *json = NULL;
    assert(localization_config_to_json(&config, "concrete", budget_ms, &json) == ESP_OK);
    assert(json != NULL && strstr(json, "\"material\":\"concrete\"") != NULL);
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    localization_calculate_config_t parsed;
    uint32_t parsed_budget = 0U;
    assert(localization_config_from_json(root, &parsed, &parsed_budget) == ESP_OK);
    assert(parsed.propagation_speed_mps == 4000.0);
    assert(parsed.propagation_speed_uncertainty_mps == 500.0);
    assert(strcmp(parsed.material, "concrete") == 0);
    assert(parsed.pick_config.sta_window_samples == 10U);
    assert(parsed.pick_config.lta_window_samples == 100U);
    assert(parsed_budget == LOCALIZATION_JOB_DEFAULT_BUDGET_MS);
    cJSON_Delete(root);
    cJSON_free(json);

    char *standard_json = NULL;
    assert(localization_config_standard_processing_to_json(&standard_json) == ESP_OK);
    assert(strstr(standard_json, "\"s_pick_enabled\":false") != NULL);
    assert(strstr(standard_json, "\"time_budget_s\":60") != NULL);
    cJSON_free(standard_json);
}

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
        "\"propagation_model_name\":\"tap-2026\",\"material\":\"custom\","
        "\"time_budget_s\":30}"
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
    assert(strcmp(config.material, "custom") == 0);
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
    assert(config.material[0] == '\0');
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
    test_material_preset_values();
    test_standard_processing_is_valid_and_round_trips();
    test_full_settings_golden();
    test_no_defaults_and_incomplete_s_rejected();
    return 0;
}
