#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "localization_energy_baseline.h"


static void test_two_equal_energy_channels_centroid_at_midpoint(void)
{
    const localization_energy_baseline_channel_t channels[2] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 },
        { .sensor_position = { 4.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 }
    };

    localization_energy_baseline_result_t result;
    assert(localization_energy_baseline_estimate(channels, 2u, &result) == ESP_OK);

    assert(result.found);
    assert(fabs(result.position.x_m - 2.0) < 1e-9);
    assert(fabs(result.position.y_m - 0.0) < 1e-9);
    assert(fabs(result.energy_concentration - 0.5) < 1e-9);
    assert(result.channels_used_count == 2u);
}


static void test_dominant_channel_pulls_centroid_toward_it(void)
{
    const localization_energy_baseline_channel_t channels[2] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 9.0, .weight = 1.0 },
        { .sensor_position = { 4.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 }
    };

    localization_energy_baseline_result_t result;
    assert(localization_energy_baseline_estimate(channels, 2u, &result) == ESP_OK);

    assert(result.found);
    /* (9*0 + 1*4) / 10 = 0.4 -- much closer to the loud channel at 0. */
    assert(fabs(result.position.x_m - 0.4) < 1e-9);
    assert(fabs(result.energy_concentration - 0.9) < 1e-9);
}


static void test_single_channel_returns_its_own_position(void)
{
    const localization_energy_baseline_channel_t channels[1] = {
        { .sensor_position = { 1.5, 2.5, 0.5 }, .energy = 3.0, .weight = 1.0 }
    };

    localization_energy_baseline_result_t result;
    assert(localization_energy_baseline_estimate(channels, 1u, &result) == ESP_OK);

    assert(result.found);
    assert(fabs(result.position.x_m - 1.5) < 1e-9);
    assert(fabs(result.position.y_m - 2.5) < 1e-9);
    assert(fabs(result.position.z_m - 0.5) < 1e-9);
    assert(result.energy_concentration == 1.0);
    assert(result.channels_used_count == 1u);
}


static void test_excluded_channel_does_not_affect_centroid(void)
{
    const localization_energy_baseline_channel_t with_excluded[3] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 },
        { .sensor_position = { 4.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 },
        { .sensor_position = { 100.0, 100.0, 100.0 }, .energy = 1000.0, .weight = 0.0 }
    };
    const localization_energy_baseline_channel_t without_excluded[2] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 },
        { .sensor_position = { 4.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 }
    };

    localization_energy_baseline_result_t with_result;
    localization_energy_baseline_result_t without_result;
    assert(
        localization_energy_baseline_estimate(with_excluded, 3u, &with_result) == ESP_OK
    );
    assert(
        localization_energy_baseline_estimate(without_excluded, 2u, &without_result) ==
        ESP_OK
    );

    assert(with_result.channels_used_count == 2u);
    assert(fabs(with_result.position.x_m - without_result.position.x_m) < 1e-9);
    assert(fabs(with_result.energy_concentration - without_result.energy_concentration) < 1e-9);
}


static void test_all_zero_energy_is_not_found(void)
{
    const localization_energy_baseline_channel_t channels[2] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 0.0, .weight = 1.0 },
        { .sensor_position = { 4.0, 0.0, 0.0 }, .energy = 0.0, .weight = 1.0 }
    };

    localization_energy_baseline_result_t result;
    assert(localization_energy_baseline_estimate(channels, 2u, &result) == ESP_OK);
    assert(!result.found);
    assert(result.channels_used_count == 2u);
}


static void test_all_channels_excluded_is_not_found(void)
{
    const localization_energy_baseline_channel_t channels[2] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 1.0, .weight = 0.0 },
        { .sensor_position = { 4.0, 0.0, 0.0 }, .energy = 1.0, .weight = 0.0 }
    };

    localization_energy_baseline_result_t result;
    assert(localization_energy_baseline_estimate(channels, 2u, &result) == ESP_OK);
    assert(!result.found);
    assert(result.channels_used_count == 0u);
}


static void test_rejects_negative_energy_and_invalid_arguments(void)
{
    const localization_energy_baseline_channel_t negative_energy[1] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = -1.0, .weight = 1.0 }
    };
    localization_energy_baseline_result_t result;
    assert(
        localization_energy_baseline_estimate(negative_energy, 1u, &result) ==
        ESP_ERR_INVALID_ARG
    );

    const localization_energy_baseline_channel_t valid[1] = {
        { .sensor_position = { 0.0, 0.0, 0.0 }, .energy = 1.0, .weight = 1.0 }
    };
    assert(
        localization_energy_baseline_estimate(NULL, 1u, &result) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_energy_baseline_estimate(valid, 0u, &result) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_energy_baseline_estimate(
            valid, LOCALIZATION_ENERGY_BASELINE_MAX_CHANNELS + 1u, &result
        ) == ESP_ERR_INVALID_ARG
    );
    assert(localization_energy_baseline_estimate(valid, 1u, NULL) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_two_equal_energy_channels_centroid_at_midpoint();
    test_dominant_channel_pulls_centroid_toward_it();
    test_single_channel_returns_its_own_position();
    test_excluded_channel_does_not_affect_centroid();
    test_all_zero_energy_is_not_found();
    test_all_channels_excluded_is_not_found();
    test_rejects_negative_energy_and_invalid_arguments();
    puts("localization energy baseline tests passed");
    return 0;
}
