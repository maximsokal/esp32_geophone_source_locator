#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "localization.h"

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

    return (int64_t)llround(
        (emission_s + distance / speed_mps) * 1000000.0
    );
}

static void assert_near(double actual, double expected, double epsilon)
{
    assert(fabs(actual - expected) <= epsilon);
}

static void test_synthetic_ground_truth(void)
{
    const localization_point3d_t source = { 1.35, 0.85, 1.10 };
    const localization_point3d_t sensors[] = {
        { 0.0, 0.0, 0.0 },
        { 4.0, 0.0, 0.0 },
        { 0.0, 3.0, 0.0 },
        { 0.0, 0.0, 3.0 },
        { 4.0, 3.0, 2.0 },
        { 2.0, 1.0, 4.0 },
    };
    const double speed_mps = 1850.0;
    const double emission_s = 123.456789;
    localization_observation_t observations[
        sizeof(sensors) / sizeof(sensors[0])
    ];

    for (size_t i = 0; i < sizeof(sensors) / sizeof(sensors[0]); ++i) {
        observations[i].sensor_position = sensors[i];
        observations[i].arrival_time_us = arrival_us(
            source,
            sensors[i],
            emission_s,
            speed_mps
        );
    }

    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = speed_mps;
    config.max_iterations = 80;
    config.convergence_m = 1.0e-8;

    const localization_point3d_t initial_guess = { 2.0, 1.5, 2.0 };
    localization_result_t result;
    assert(localization_tdoa_solve(
        observations,
        sizeof(observations) / sizeof(observations[0]),
        &config,
        &initial_guess,
        &result
    ) == ESP_OK);

    assert(result.converged);
    /* Integer-microsecond picks at 1850 m/s quantize distance by 1.85 mm. */
    assert_near(result.source_position.x_m, source.x_m, 0.0025);
    assert_near(result.source_position.y_m, source.y_m, 0.0025);
    assert_near(result.source_position.z_m, source.z_m, 0.0025);
    assert(result.rms_residual_us <= 1.0);
    assert_near(
        (double)result.emission_time_us,
        emission_s * 1000000.0,
        1.0
    );
}

static void test_four_sensor_offset_noise_and_iteration_contracts(void)
{
    const localization_point3d_t source = { 0.25, 0.40, 0.30 };
    const localization_point3d_t sensors[] = {
        { 0.0, 0.0, 0.0 },
        { 2.0, 0.0, 0.0 },
        { 0.0, 2.0, 0.0 },
        { 0.0, 0.0, 2.0 },
        { 2.0, 2.0, 2.0 },
    };
    const double speed_mps = 340.0;
    const int64_t offset_us = 4000000000000LL;
    localization_observation_t observations[5];

    for (size_t i = 0; i < 5; ++i) {
        observations[i].sensor_position = sensors[i];
        observations[i].arrival_time_us = offset_us + arrival_us(
            source, sensors[i], 12.0, speed_mps
        );
    }

    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = speed_mps;
    config.max_iterations = 100;
    config.convergence_m = 1.0e-7;
    localization_result_t result;

    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_OK);
    assert(result.converged);
    assert_near(result.source_position.x_m, source.x_m, 0.01);
    assert_near(result.source_position.y_m, source.y_m, 0.01);
    assert_near(result.source_position.z_m, source.z_m, 0.01);

    observations[2].arrival_time_us += 2;
    assert(localization_tdoa_solve(
        observations, 5, &config, NULL, &result
    ) == ESP_OK);
    assert(result.rms_residual_us > 0.0);
    assert(result.rms_residual_us < 3.0);

    config.max_iterations = 1;
    config.convergence_m = 1.0e-12;
    const localization_point3d_t distant_guess = { 10.0, 10.0, 10.0 };
    assert(localization_tdoa_solve(
        observations, 5, &config, &distant_guess, &result
    ) == ESP_OK);
    assert(!result.converged);
    assert(result.iterations == 1);
}

static void test_validation_and_geometry_contracts(void)
{
    localization_config_t config = localization_default_config();
    localization_result_t result;
    localization_observation_t observations[4] = {
        { { 0.0, 0.0, 0.0 }, 0 },
        { { 1.0, 0.0, 0.0 }, 0 },
        { { 0.0, 1.0, 0.0 }, 0 },
        { { 0.0, 0.0, 1.0 }, 0 },
    };

    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);

    config.propagation_speed_mps = 1000.0;
    assert(localization_tdoa_solve(NULL, 4, &config, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_tdoa_solve(observations, 4, NULL, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_tdoa_solve(observations, 4, &config, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_tdoa_solve(
        observations, 3, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);

    config.propagation_speed_mps = NAN;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);
    config.propagation_speed_mps = 1000.0;

    config.convergence_m = NAN;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);
    config.convergence_m = 1.0e-3;

    config.damping = NAN;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);
    config.damping = 1.0e-6;

    observations[3].sensor_position.z_m = 0.0;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_STATE);

    observations[3].sensor_position.z_m = 1.0;
    observations[1].sensor_position.x_m = INFINITY;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);
    observations[1].sensor_position.x_m = 1.0;
    observations[0].arrival_time_us = 0;
    observations[1].arrival_time_us = 1000;
    observations[2].arrival_time_us = 1000;
    observations[3].arrival_time_us = 1000;
    localization_point3d_t bad_guess = { NAN, 0.0, 0.0 };
    assert(localization_tdoa_solve(
        observations, 4, &config, &bad_guess, &result
    ) == ESP_ERR_INVALID_ARG);

    const localization_point3d_t sensor_guess = observations[0].sensor_position;
    assert(localization_tdoa_solve(
        observations, 4, &config, &sensor_guess, &result
    ) == ESP_ERR_INVALID_STATE);

    config.damping = -1.0;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);

    config.damping = 1.0e-6;
    assert(localization_tdoa_solve(
        observations, LOCALIZATION_MAX_SENSORS + 1U,
        &config, NULL, &result
    ) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_synthetic_ground_truth();
    test_four_sensor_offset_noise_and_iteration_contracts();
    test_validation_and_geometry_contracts();
    puts("localization TDOA tests passed");
    return 0;
}
