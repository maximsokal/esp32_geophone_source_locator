#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "localization_config.h"
#include "localization_job.h"
#include "localization_material_presets.h"

static cJSON *settings_without_velocity_range(const char *material, double speed, double uncertainty)
{
    char json[768];
    (void)snprintf(json, sizeof(json),
        "{\"material\":\"%s\",\"propagation_speed_mps\":%.17g,"
        "\"propagation_speed_uncertainty_mps\":%.17g,\"timing_uncertainty_us\":250,"
        "\"noise_window_samples\":80,\"sta_window_samples\":20,\"lta_window_samples\":200,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":12,"
        "\"refine_lookahead_samples\":24,\"refine_amplitude_fraction\":0.18,"
        "\"snr_signal_window_samples\":40,\"minimum_snr\":2.25,"
        "\"semblance_window_half_span_samples\":8}", material, speed, uncertainty);
    return cJSON_Parse(json);
}

static cJSON *settings_for_scalar(double speed, double uncertainty)
{
    return settings_without_velocity_range("custom", speed, uncertainty);
}

static cJSON *legacy_settings_without_velocity_range(void)
{
    return settings_without_velocity_range("custom", 340.0, 12.0);
}

static void add_velocity_range(
    cJSON *root,
    double v_min,
    double v_max,
    double vp_vs_min,
    double vp_vs_max,
    const char *source
)
{
    cJSON *range = cJSON_AddObjectToObject(root, "velocity_range");
    assert(range != NULL);
    assert(cJSON_AddNumberToObject(range, "v_min_mps", v_min) != NULL);
    assert(cJSON_AddNumberToObject(range, "v_max_mps", v_max) != NULL);
    assert(cJSON_AddNumberToObject(range, "vp_vs_min", vp_vs_min) != NULL);
    assert(cJSON_AddNumberToObject(range, "vp_vs_max", vp_vs_max) != NULL);
    assert(cJSON_AddStringToObject(range, "source", source) != NULL);
}

static void test_material_preset_values(void)
{
    static const localization_material_preset_t expected[] = {
        { "concrete", 4000U, 500U, 3000.0f, 4800.0f, 1.55f, 1.80f },
        { "reinforced_concrete", 4200U, 500U, 3200.0f, 5000.0f, 1.55f, 1.80f },
        { "brick_masonry", 2200U, 700U, 1200.0f, 3500.0f, 1.50f, 1.90f },
        { "rubble", 400U, 250U, 150.0f, 1500.0f, 1.50f, 2.50f },
        { "steel", 5200U, 700U, 4800.0f, 6000.0f, 1.75f, 1.95f },
        { "soil_dry", 500U, 300U, 150.0f, 800.0f, 1.50f, 2.50f },
        { "soil_saturated", 1500U, 500U, 1400.0f, 2000.0f, 2.50f, 8.00f },
    };
    assert(localization_material_preset_count() == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0U; i < localization_material_preset_count(); ++i) {
        const localization_material_preset_t *actual = localization_material_preset_at(i);
        assert(actual != NULL);
        assert(strcmp(actual->id, expected[i].id) == 0);
        assert(actual->propagation_speed_mps == expected[i].propagation_speed_mps);
        assert(actual->propagation_speed_uncertainty_mps == expected[i].propagation_speed_uncertainty_mps);
        assert(actual->v_min_mps == expected[i].v_min_mps);
        assert(actual->v_max_mps == expected[i].v_max_mps);
        assert(actual->vp_vs_min == expected[i].vp_vs_min);
        assert(actual->vp_vs_max == expected[i].vp_vs_max);
        const localization_velocity_bounds_t bounds = {
            .v_min_mps = actual->v_min_mps,
            .v_max_mps = actual->v_max_mps,
            .vp_vs_min = actual->vp_vs_min,
            .vp_vs_max = actual->vp_vs_max,
            .set = true,
        };
        assert(localization_velocity_bounds_validate(&bounds) == LOCALIZATION_VELOCITY_BOUNDS_VALID);
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
    assert(config.sample_rate_hz == 500U);
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
    assert(parsed.sample_rate_hz == 500U);
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
    assert(strstr(standard_json, "\"sample_rate_hz\":500") != NULL);
    cJSON_free(standard_json);
}

/* Compatibility: legacy calc_settings.json without velocity_range still loads. */
static void test_compat_legacy_settings_load_without_velocity_range(void)
{
    cJSON *root = legacy_settings_without_velocity_range();
    assert(root != NULL);
    localization_calculate_config_t config;
    uint32_t budget_ms = 99U;
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
    assert(config.propagation_speed_mps == 340.0);
    assert(config.propagation_speed_uncertainty_mps == 12.0);
    assert(budget_ms == 0U);
    cJSON_Delete(root);
}

static void test_legacy_settings_normalize_range_and_serialize_deterministically(void)
{
    char *serialized[2] = { NULL, NULL };
    for (size_t i = 0U; i < 2U; ++i) {
        cJSON *root = legacy_settings_without_velocity_range();
        assert(root != NULL);
        localization_calculate_config_t config;
        uint32_t budget_ms = 0U;
        assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
        cJSON_Delete(root);
        assert(localization_config_to_json(&config, "custom", budget_ms, &serialized[i]) == ESP_OK);
    }
    assert(strcmp(serialized[0], serialized[1]) == 0);

    const char *first = serialized[0];
    cJSON *normalized = cJSON_Parse(first);
    assert(normalized != NULL);
    const cJSON *range = cJSON_GetObjectItemCaseSensitive(normalized, "velocity_range");
    assert(cJSON_IsObject(range));
    assert(cJSON_GetObjectItemCaseSensitive(range, "v_min_mps")->valuedouble == 316.0);
    assert(cJSON_GetObjectItemCaseSensitive(range, "v_max_mps")->valuedouble == 364.0);
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(range, "source");
    assert(cJSON_IsString(source) && strcmp(source->valuestring, "scalar_derived") == 0);

    cJSON_Delete(normalized);
    cJSON_free(serialized[0]);
    cJSON_free(serialized[1]);
}

static void test_scalar_derived_ranges_contain_every_sampled_legacy_speed(void)
{
    static const double speeds[] = {
        50.000001, 50.001, 51.0, 100.0, 500.0, 2500.0, 7999.999, 7999.999999,
    };
    for (size_t i = 0U; i < sizeof(speeds) / sizeof(speeds[0]); ++i) {
        const double uncertainties[] = { speeds[i] * 1.0e-10, speeds[i] * 0.01, speeds[i] * 0.99 };
        for (size_t j = 0U; j < sizeof(uncertainties) / sizeof(uncertainties[0]); ++j) {
            cJSON *root = settings_for_scalar(speeds[i], uncertainties[j]);
            assert(root != NULL);
            localization_calculate_config_t config;
            uint32_t budget_ms = 0U;
            assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
            cJSON_Delete(root);
            assert(config.propagation_speed_mps == speeds[i]);
            assert(config.propagation_speed_uncertainty_mps == uncertainties[j]);
            assert(config.velocity_range.source == LOCALIZATION_VELOCITY_RANGE_SOURCE_SCALAR_DERIVED);
            assert(config.velocity_range.bounds.set);
            assert(config.velocity_range.bounds.v_min_mps <= speeds[i]);
            assert(config.velocity_range.bounds.v_max_mps >= speeds[i]);
            assert((double)config.velocity_range.bounds.v_max_mps /
                config.velocity_range.bounds.v_min_mps <= 10.0);
            assert(localization_velocity_bounds_validate(&config.velocity_range.bounds) ==
                LOCALIZATION_VELOCITY_BOUNDS_VALID);
        }
    }
}

static void test_explicit_range_precedes_preset_and_normalizes_source(void)
{
    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    cJSON *root = settings_without_velocity_range("concrete", 4000.0, 500.0);
    assert(root != NULL);
    add_velocity_range(root, 2900.0, 4400.0, 1.60, 1.90, "scalar_derived");
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
    assert(config.velocity_range.source == LOCALIZATION_VELOCITY_RANGE_SOURCE_USER);
    assert(config.velocity_range.bounds.v_min_mps == 2900.0f);
    assert(config.velocity_range.bounds.v_max_mps == 4400.0f);
    char *serialized = NULL;
    assert(localization_config_to_json(&config, "concrete", budget_ms, &serialized) == ESP_OK);
    cJSON *serialized_root = cJSON_Parse(serialized);
    assert(serialized_root != NULL);
    const cJSON *serialized_range = cJSON_GetObjectItemCaseSensitive(serialized_root, "velocity_range");
    assert(cJSON_IsObject(serialized_range));
    const cJSON *serialized_source = cJSON_GetObjectItemCaseSensitive(serialized_range, "source");
    assert(cJSON_IsString(serialized_source) && strcmp(serialized_source->valuestring, "user") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(serialized_range, "v_min_mps")->valuedouble == 2900.0);
    cJSON_Delete(serialized_root);
    cJSON_free(serialized);
    cJSON_Delete(root);

    root = settings_without_velocity_range("concrete", 4000.0, 500.0);
    assert(root != NULL);
    add_velocity_range(root, 3000.0, 4800.0, 1.55, 1.80, "user");
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
    assert(config.velocity_range.source == LOCALIZATION_VELOCITY_RANGE_SOURCE_PRESET);
    cJSON_Delete(root);

    root = settings_without_velocity_range("concrete", 4000.0, 500.0);
    assert(root != NULL);
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_OK);
    assert(config.velocity_range.source == LOCALIZATION_VELOCITY_RANGE_SOURCE_PRESET);
    assert(config.velocity_range.bounds.v_min_mps == 3000.0f);
    assert(config.velocity_range.bounds.v_max_mps == 4800.0f);
    cJSON_Delete(root);
}

static void test_incomplete_vp_vs_range_is_rejected(void)
{
    cJSON *root = legacy_settings_without_velocity_range();
    assert(root != NULL);
    add_velocity_range(root, 300.0, 400.0, 1.5, 0.0, "user");
    localization_calculate_config_t config;
    uint32_t budget_ms = 0U;
    assert(localization_config_from_json(root, &config, &budget_ms) == ESP_ERR_INVALID_ARG);
    cJSON_Delete(root);
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
        "\"semblance_window_half_span_samples\":8,\"sample_rate_hz\":20000,\"max_source_depth_m\":6.5,"
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
    assert(config.sample_rate_hz == 20000U);
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
    assert(config.sample_rate_hz == 500U); /* legacy settings have no recorded rate */
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


static void test_invalid_sample_rate_is_rejected(void)
{
    const char *prefix =
        "{\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":12,"
        "\"timing_uncertainty_us\":250,\"noise_window_samples\":80,"
        "\"sta_window_samples\":20,\"lta_window_samples\":200,"
        "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":12,"
        "\"refine_lookahead_samples\":24,\"refine_amplitude_fraction\":0.18,"
        "\"snr_signal_window_samples\":40,\"minimum_snr\":2.25,"
        "\"semblance_window_half_span_samples\":8,\"sample_rate_hz\":";
    const char *invalid[] = { "0", "1.5", "4294967296" };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char json[512];
        (void)snprintf(json, sizeof(json), "%s%s}", prefix, invalid[i]);
        cJSON *root = cJSON_Parse(json);
        assert(root != NULL);
        localization_calculate_config_t config;
        uint32_t budget_ms = 0U;
        assert(localization_config_from_json(root, &config, &budget_ms) == ESP_ERR_INVALID_ARG);
        cJSON_Delete(root);
    }
}

int main(void)
{
    test_material_preset_values();
    test_standard_processing_is_valid_and_round_trips();
    test_compat_legacy_settings_load_without_velocity_range();
    test_legacy_settings_normalize_range_and_serialize_deterministically();
    test_scalar_derived_ranges_contain_every_sampled_legacy_speed();
    test_explicit_range_precedes_preset_and_normalizes_source();
    test_incomplete_vp_vs_range_is_rejected();
    test_full_settings_golden();
    test_no_defaults_and_incomplete_s_rejected();
    test_invalid_sample_rate_is_rejected();
    return 0;
}
