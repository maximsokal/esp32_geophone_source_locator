#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "localization_timing_budget.h"


static int64_t arrival_us(
    localization_point3d_t source,
    localization_point3d_t sensor,
    double emission_s,
    double speed_mps
)
{
    const double dx = source.x_m - sensor.x_m;
    const double dy = source.y_m - sensor.y_m;
    const double dz = source.z_m - sensor.z_m;
    const double distance = sqrt(dx * dx + dy * dy + dz * dz);

    return (int64_t)llround((emission_s + distance / speed_mps) * 1000000.0);
}


static void build_observations(
    localization_observation_t observations[4]
)
{
    const localization_point3d_t source = { 0.25, 0.40, 0.30 };
    const localization_point3d_t sensors[4] = {
        { 0.0, 0.0, 0.0 },
        { 2.0, 0.0, 0.0 },
        { 0.0, 2.0, 0.0 },
        { 0.0, 0.0, 2.0 }
    };
    const double speed_mps = 340.0;

    for (size_t i = 0u; i < 4u; ++i) {
        observations[i].sensor_position = sensors[i];
        observations[i].arrival_time_us =
            arrival_us(source, sensors[i], 0.1, speed_mps);
    }
}


static localization_config_t test_config(void)
{
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = 340.0;
    config.max_iterations = 100;
    config.convergence_m = 1.0e-7;
    return config;
}


static void test_zero_jitter_gives_zero_error(void)
{
    localization_observation_t observations[4];
    build_observations(observations);
    const localization_config_t config = test_config();
    double error_m = -1.0;

    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, &config, NULL, 0, &error_m
        ) == ESP_OK
    );
    assert(error_m < 1.0e-6);
}


static void test_error_grows_with_jitter_magnitude(void)
{
    localization_observation_t observations[4];
    build_observations(observations);
    const localization_config_t config = test_config();

    double error_1us = 0.0;
    double error_10us = 0.0;
    double error_100us = 0.0;

    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, &config, NULL, 1, &error_1us
        ) == ESP_OK
    );
    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, &config, NULL, 10, &error_10us
        ) == ESP_OK
    );
    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, &config, NULL, 100, &error_100us
        ) == ESP_OK
    );

    printf(
        "[MAX-142] timing jitter error bound: 1us=%.6f m 10us=%.6f m 100us=%.6f m\n",
        error_1us, error_10us, error_100us
    );

    assert(error_1us < error_10us);
    assert(error_10us < error_100us);
}


static void test_2ms_jitter_matches_the_max_141_measured_500hz_finding(void)
{
    /* Cross-check against MAX-141's real measured finding: whole-sample
     * picking at the project's ~500 Hz rate quantizes to 2 ms and produced
     * ~0.375 m of position error end-to-end through the real pick+solve
     * pipeline. This is a different (perturbation-based) method computing
     * the same physical quantity; it should land in the same order of
     * magnitude, not an unrelated number. */
    localization_observation_t observations[4];
    build_observations(observations);
    const localization_config_t config = test_config();
    double error_m = 0.0;

    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, &config, NULL, 2000, &error_m
        ) == ESP_OK
    );

    printf("[MAX-142] 2ms jitter bound: %.4f m (MAX-141 measured ~0.375 m)\n", error_m);
    assert(error_m > 0.05 && error_m < 2.0);
}


static void test_rejects_invalid_arguments(void)
{
    localization_observation_t observations[4];
    build_observations(observations);
    const localization_config_t config = test_config();
    double error_m = 0.0;

    assert(
        localization_timing_jitter_error_bound(
            NULL, 4u, &config, NULL, 100, &error_m
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, NULL, NULL, 100, &error_m
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_timing_jitter_error_bound(
            observations, 4u, &config, NULL, 100, NULL
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_timing_jitter_error_bound(
            observations, 0u, &config, NULL, 100, &error_m
        ) == ESP_ERR_INVALID_ARG
    );
}


int main(void)
{
    test_zero_jitter_gives_zero_error();
    test_error_grows_with_jitter_magnitude();
    test_2ms_jitter_matches_the_max_141_measured_500hz_finding();
    test_rejects_invalid_arguments();
    puts("localization timing budget tests passed");
    return 0;
}
