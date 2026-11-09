#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

/* MAX-211: a practically flat surface array whose only vertical "relief" is
 * barometric noise (a few cm over 5 m) must not be accepted as 3D -- the
 * solved depth would be determined by that noise. */
static void test_flat_array_with_height_noise_is_rejected(void)
{
    localization_observation_t observations[4] = {
        { .sensor_position = { 0.0, 0.0, 0.00 }, .arrival_time_us = 0 },
        { .sensor_position = { 5.0, 0.0, 0.05 }, .arrival_time_us = 1000 },
        { .sensor_position = { 0.0, 5.0, -0.03 }, .arrival_time_us = 1200 },
        { .sensor_position = { 5.0, 5.0, 0.02 }, .arrival_time_us = 1500 },
    };
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = 1000.0;
    localization_result_t result;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_ERR_INVALID_STATE);

    /* ~1 m of real relief across the same footprint is depth-resolving. */
    observations[3].sensor_position.z_m = 1.0;
    assert(localization_tdoa_solve(
        observations, 4, &config, NULL, &result
    ) == ESP_OK);
}


static void test_positions_resolve_depth_is_order_independent(void)
{
    /* Four surface sensors with a few cm of barometric Z noise: flat. */
    const localization_point3d_t flat[5] = {
        { 0.0, 0.0, 0.00 }, { 5.0, 0.0, 0.03 }, { 0.0, 5.0, -0.02 }, { 5.0, 5.0, 0.05 }, { 2.5, 2.5, 0.01 },
    };
    assert(!localization_positions_resolve_depth(flat, 5U));

    /* Real relief exists only among the last four: found whichever sensor
     * is listed first (the TDOA solver's own check fixes the first one). */
    const localization_point3d_t relief[5] = {
        { 1.0, 1.0, 0.0 }, { 1.0, 1.0, 0.0 }, { 5.0, 0.0, 0.0 }, { 0.0, 5.0, 0.0 }, { 2.0, 2.0, 3.0 },
    };
    const localization_point3d_t reordered[5] = {
        relief[4], relief[2], relief[3], relief[1], relief[0],
    };
    assert(localization_positions_resolve_depth(relief, 5U));
    assert(localization_positions_resolve_depth(reordered, 5U));

    /* Fewer than four points, non-finite input and NULL never resolve. */
    assert(!localization_positions_resolve_depth(relief + 2, 3U));
    const localization_point3d_t broken[4] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, NAN } };
    assert(!localization_positions_resolve_depth(broken, 4U));
    assert(!localization_positions_resolve_depth(NULL, 4U));
}

/*
 * Over-determined layout (6 sensors, 3 unknowns) with noisy picks, shared by
 * the two maximum-likelihood tests below. Noise is deterministic (whole
 * microseconds), |n| <= 40 us at 300 m/s.
 */
#define ML_SENSOR_COUNT 6U

static const localization_point3d_t g_ml_sensors[ML_SENSOR_COUNT] = {
    { 0.0, 0.0, 0.0 }, { 4.0, 0.0, 0.0 }, { 0.0, 4.0, 0.0 },
    { 4.0, 4.0, 0.3 }, { 2.0, 2.0, 2.5 }, { 0.5, 3.5, 1.2 },
};
static const int64_t g_ml_noise_us[ML_SENSOR_COUNT] = { 40, -25, 10, -35, 30, -15 };
static const localization_point3d_t g_ml_source = { 1.3, 2.2, 1.0 };
static const double g_ml_speed_mps = 300.0;

static void ml_observations(
    const size_t *order,
    localization_observation_t observations[ML_SENSOR_COUNT]
)
{
    for (size_t k = 0U; k < ML_SENSOR_COUNT; ++k) {
        const size_t i = order[k];
        observations[k].sensor_position = g_ml_sensors[i];
        observations[k].arrival_time_us =
            arrival_us(g_ml_source, g_ml_sensors[i], 0.010, g_ml_speed_mps) + g_ml_noise_us[i];
    }
}

static localization_config_t ml_config(void)
{
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = g_ml_speed_mps;
    config.max_iterations = 100U;
    config.convergence_m = 1.0e-10;
    return config;
}

/*
 * Chan & Ho (1994), IEEE Trans. Signal Process. 42(8):1905-1915,
 * doi:10.1109/78.301830 (covariance and C^-1 weight written out in
 * arXiv:2502.19354, eqs. 10 and 71): differences formed
 * against a reference sensor share that sensor's arrival-time error, so for
 * i.i.d. pick noise their covariance is Q = sigma^2 (I + 1 1^T) and the
 * maximum-likelihood estimate minimises r^T Q^-1 r. That objective is a
 * property of the picks, not of the order they are listed in: renaming the
 * reference sensor must not move the estimate. An identity weight
 * (r^T r) moves it by millimetres here.
 */
static void test_overdetermined_solution_does_not_depend_on_reference_sensor(void)
{
    static const size_t orders[3][ML_SENSOR_COUNT] = {
        { 0U, 1U, 2U, 3U, 4U, 5U },
        { 3U, 0U, 1U, 2U, 4U, 5U },
        { 5U, 1U, 2U, 3U, 4U, 0U },
    };
    const localization_config_t config = ml_config();
    localization_result_t results[3];

    for (size_t o = 0U; o < 3U; ++o) {
        localization_observation_t observations[ML_SENSOR_COUNT];
        ml_observations(orders[o], observations);
        assert(localization_tdoa_solve(observations, ML_SENSOR_COUNT, &config, NULL, &results[o]) == ESP_OK);
        assert(results[o].converged);
    }

    for (size_t o = 1U; o < 3U; ++o) {
        const double dx = results[o].source_position.x_m - results[0].source_position.x_m;
        const double dy = results[o].source_position.y_m - results[0].source_position.y_m;
        const double dz = results[o].source_position.z_m - results[0].source_position.z_m;
        const double shift_m = sqrt(dx * dx + dy * dy + dz * dz);
        printf("[TDOA-ML] reference sensor %zu vs 0: estimate moved %.3e m\n", orders[o][0], shift_m);
        assert(shift_m < 1.0e-6);
    }
}

/*
 * Origin-time elimination of probabilistic event location (NonLinLoc,
 * Lomax et al. 2000, doi:10.1007/978-94-015-9536-0_5): with Gaussian
 * pick errors d chi^2 / d t0 = 0 eliminates the origin time analytically, t0 = mean_i(t_i - D_i / v), leaving
 *   chi^2(x) = sum_i (r_i - mean r)^2,   r_i = v t_i - |x - s_i|.
 * Its maximum-likelihood point is a stationary point:
 *   grad chi^2 / 2 = -sum_i (r_i - mean r) (x - s_i) / |x - s_i| = 0.
 * The solver's answer must satisfy that equation, and its emission time
 * must be the eliminated t0 at that point.
 */
static void test_overdetermined_solution_is_stationary_point_of_origin_free_likelihood(void)
{
    static const size_t order[ML_SENSOR_COUNT] = { 0U, 1U, 2U, 3U, 4U, 5U };
    const localization_config_t config = ml_config();
    localization_observation_t observations[ML_SENSOR_COUNT];
    ml_observations(order, observations);

    localization_result_t result;
    assert(localization_tdoa_solve(observations, ML_SENSOR_COUNT, &config, NULL, &result) == ESP_OK);
    assert(result.converged);

    const localization_point3d_t x = result.source_position;
    double r[ML_SENSOR_COUNT];
    double u[ML_SENSOR_COUNT][3];
    double mean_r = 0.0;
    double mean_t0_us = 0.0;
    for (size_t i = 0U; i < ML_SENSOR_COUNT; ++i) {
        const double dx = x.x_m - observations[i].sensor_position.x_m;
        const double dy = x.y_m - observations[i].sensor_position.y_m;
        const double dz = x.z_m - observations[i].sensor_position.z_m;
        const double d = sqrt(dx * dx + dy * dy + dz * dz);
        /* Times relative to the first pick keep v t_i at metre scale. */
        const double t_s = (double)(observations[i].arrival_time_us - observations[0].arrival_time_us) * 1.0e-6;
        r[i] = g_ml_speed_mps * t_s - d;
        u[i][0] = dx / d;
        u[i][1] = dy / d;
        u[i][2] = dz / d;
        mean_r += r[i] / (double)ML_SENSOR_COUNT;
        mean_t0_us += ((double)observations[i].arrival_time_us - d / g_ml_speed_mps * 1.0e6) /
            (double)ML_SENSOR_COUNT;
    }
    double gradient[3] = { 0.0, 0.0, 0.0 };
    for (size_t i = 0U; i < ML_SENSOR_COUNT; ++i) {
        for (size_t a = 0U; a < 3U; ++a) {
            gradient[a] -= (r[i] - mean_r) * u[i][a];
        }
    }
    const double gradient_norm_m =
        sqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1] + gradient[2] * gradient[2]);
    printf("[TDOA-ML] |grad chi^2/2| at the estimate = %.3e m\n", gradient_norm_m);
    assert(gradient_norm_m < 1.0e-8);
    assert(llabs(result.emission_time_us - (int64_t)llround(mean_t0_us)) <= 1);
}

int main(void)
{
    test_overdetermined_solution_does_not_depend_on_reference_sensor();
    test_overdetermined_solution_is_stationary_point_of_origin_free_likelihood();
    test_synthetic_ground_truth();
    test_four_sensor_offset_noise_and_iteration_contracts();
    test_validation_and_geometry_contracts();
    test_flat_array_with_height_noise_is_rejected();
    test_positions_resolve_depth_is_order_independent();
    puts("localization TDOA tests passed");
    return 0;
}
