#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "localization_propagation_model.h"


static void test_calibrates_close_to_the_true_synthetic_velocity(void)
{
    /* Synthetic taps at known distances from a reference sensor, generated
     * from a true velocity of 500 m/s (a plausible rubble/debris P-wave
     * speed) plus a fixed 2 ms detection-latency intercept and small
     * deterministic per-sample residuals (not an exact fit -- real taps
     * never are). */
    const double distances_m[] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
    const double travel_times_s[] = {
        0.004, 0.00602, 0.00799, 0.01003, 0.01198, 0.01401
    };

    localization_propagation_model_t model;
    assert(
        localization_propagation_calibrate_from_taps(
            distances_m, travel_times_s, 6u, "bench_calibration", &model
        ) == ESP_OK
    );

    printf(
        "[MAX-139] calibrated velocity=%.4f m/s uncertainty=%.4f m/s\n",
        model.velocity_mps, model.uncertainty_mps
    );

    assert(strcmp(model.name, "bench_calibration") == 0);
    assert(fabs(model.velocity_mps - 500.0) < 1.0);
    assert(model.uncertainty_mps > 0.0 && model.uncertainty_mps < 5.0);
}


static void test_exact_fit_has_near_zero_uncertainty(void)
{
    /* No added residuals at all: every point lies exactly on the line. */
    const double distances_m[] = { 1.0, 2.0, 3.0, 4.0 };
    const double travel_times_s[] = { 0.002, 0.004, 0.006, 0.008 };

    localization_propagation_model_t model;
    assert(
        localization_propagation_calibrate_from_taps(
            distances_m, travel_times_s, 4u, "exact", &model
        ) == ESP_OK
    );

    assert(fabs(model.velocity_mps - 500.0) < 1.0e-6);
    assert(model.uncertainty_mps < 1.0e-6);
}


static void test_rejects_too_few_samples(void)
{
    const double distances_m[] = { 1.0, 2.0 };
    const double travel_times_s[] = { 0.002, 0.004 };
    localization_propagation_model_t model;

    assert(
        localization_propagation_calibrate_from_taps(
            distances_m, travel_times_s, 2u, "n/a", &model
        ) == ESP_ERR_INVALID_ARG
    );
}


static void test_rejects_identical_distances(void)
{
    const double distances_m[] = { 2.0, 2.0, 2.0 };
    const double travel_times_s[] = { 0.004, 0.0041, 0.0039 };
    localization_propagation_model_t model;

    assert(
        localization_propagation_calibrate_from_taps(
            distances_m, travel_times_s, 3u, "n/a", &model
        ) == ESP_ERR_INVALID_STATE
    );
}


static void test_rejects_non_finite_or_negative_input(void)
{
    const double bad_distance[] = { 1.0, -2.0, 3.0 };
    const double times_ok[] = { 0.002, 0.004, 0.006 };
    localization_propagation_model_t model;

    assert(
        localization_propagation_calibrate_from_taps(
            bad_distance, times_ok, 3u, "n/a", &model
        ) == ESP_ERR_INVALID_ARG
    );
}


static void test_rejects_null_and_oversized_arguments(void)
{
    const double distances_m[] = { 1.0, 2.0, 3.0 };
    const double travel_times_s[] = { 0.002, 0.004, 0.006 };
    localization_propagation_model_t model;

    assert(
        localization_propagation_calibrate_from_taps(
            NULL, travel_times_s, 3u, "n/a", &model
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_propagation_calibrate_from_taps(
            distances_m, travel_times_s, 3u, "n/a", NULL
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_propagation_calibrate_from_taps(
            distances_m, travel_times_s, 1000u, "n/a", &model
        ) == ESP_ERR_INVALID_ARG
    );
}


static localization_observation_t s_observations[4];


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


static void build_observations(void)
{
    const localization_point3d_t source = { 0.25, 0.40, 0.30 };
    const localization_point3d_t sensors[4] = {
        { 0.0, 0.0, 0.0 },
        { 2.0, 0.0, 0.0 },
        { 0.0, 2.0, 0.0 },
        { 0.0, 0.0, 2.0 }
    };
    for (size_t i = 0u; i < 4u; ++i) {
        s_observations[i].sensor_position = sensors[i];
        s_observations[i].arrival_time_us =
            arrival_us(source, sensors[i], 0.1, 340.0);
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


static void test_velocity_error_bound_grows_with_mismatch_magnitude(void)
{
    build_observations();
    const localization_config_t config = test_config();

    double error_small = 0.0;
    double error_large = 0.0;

    assert(
        localization_propagation_velocity_error_bound(
            s_observations, 4u, &config, NULL, 5.0, &error_small
        ) == ESP_OK
    );
    assert(
        localization_propagation_velocity_error_bound(
            s_observations, 4u, &config, NULL, 40.0, &error_large
        ) == ESP_OK
    );

    printf(
        "[MAX-139] velocity error bound: +5 m/s=%.6f m +40 m/s=%.6f m\n",
        error_small, error_large
    );

    assert(error_small >= 0.0);
    assert(error_small < error_large);
}


static void test_zero_velocity_error_gives_zero_position_error(void)
{
    build_observations();
    const localization_config_t config = test_config();
    double error_m = -1.0;

    assert(
        localization_propagation_velocity_error_bound(
            s_observations, 4u, &config, NULL, 0.0, &error_m
        ) == ESP_OK
    );
    assert(error_m < 1.0e-6);
}


static void test_velocity_error_bound_rejects_invalid_arguments(void)
{
    build_observations();
    const localization_config_t config = test_config();
    double error_m = 0.0;

    assert(
        localization_propagation_velocity_error_bound(
            NULL, 4u, &config, NULL, 5.0, &error_m
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_propagation_velocity_error_bound(
            s_observations, 4u, &config, NULL, 5.0, NULL
        ) == ESP_ERR_INVALID_ARG
    );
    /* A perturbation that would drive the assumed speed to <= 0 is rejected
     * rather than handed to the solver as a nonphysical value. */
    assert(
        localization_propagation_velocity_error_bound(
            s_observations, 4u, &config, NULL, -340.0, &error_m
        ) == ESP_ERR_INVALID_ARG
    );
}


int main(void)
{
    test_calibrates_close_to_the_true_synthetic_velocity();
    test_exact_fit_has_near_zero_uncertainty();
    test_rejects_too_few_samples();
    test_rejects_identical_distances();
    test_rejects_non_finite_or_negative_input();
    test_rejects_null_and_oversized_arguments();
    test_velocity_error_bound_grows_with_mismatch_magnitude();
    test_zero_velocity_error_gives_zero_position_error();
    test_velocity_error_bound_rejects_invalid_arguments();
    puts("localization propagation model tests passed");
    return 0;
}
