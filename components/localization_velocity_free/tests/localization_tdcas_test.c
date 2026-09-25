#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization.h"
#include "localization_tdcas.h"

/*
 * TDCAS (Dong et al., IEEE Access 2017): P-arrival-only, velocity-free,
 * analytical 6-sensor solution + logistic-PDF "comprehensive" fusion over
 * every combination. Synthetic ground truth throughout.
 */

static double distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


static int64_t arrival_us(localization_point3d_t source, localization_point3d_t sensor, double emission_s, double v)
{
    return (int64_t)llround((emission_s + distance(source, sensor) / v) * 1.0e6);
}


/* Deterministic pick noise in [-amplitude, +amplitude] microseconds. */
static int64_t noise_us(uint32_t *state, double amplitude_us)
{
    *state = *state * 1664525U + 1013904223U;
    const double unit = (double)(*state >> 8) / (double)(1U << 24);
    return (int64_t)llround((2.0 * unit - 1.0) * amplitude_us);
}


static void make_observations(
    const localization_point3d_t *sensors,
    size_t count,
    localization_point3d_t source,
    double emission_s,
    double v,
    localization_observation_t *out
)
{
    for (size_t i = 0U; i < count; ++i) {
        out[i].sensor_position = sensors[i];
        out[i].arrival_time_us = arrival_us(source, sensors[i], emission_s, v);
    }
}


/* A depth-resolving layout: sensors on a rubble slope plus two in shafts. */
static const localization_point3d_t SENSORS_3D[8] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.3 },
    { 0.0, 3.5, 0.6 },
    { 4.0, 3.5, 1.4 },
    { 2.0, 1.5, 2.2 },
    { 1.0, 2.8, -0.9 },
    { 3.1, 0.7, -0.6 },
    { 2.6, 3.0, 0.9 },
};

/* A flat surface array (all z = 0): the realistic geophone layout. */
static const localization_point3d_t SENSORS_PLANAR[8] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.0 },
    { 0.0, 4.0, 0.0 },
    { 4.0, 4.0, 0.0 },
    { 2.1, 0.9, 0.0 },
    { 0.8, 2.3, 0.0 },
    { 3.3, 2.6, 0.0 },
    { 1.9, 3.4, 0.0 },
};


static void test_six_sensor_explicit_solution(void)
{
    const localization_point3d_t source = { 1.7, 1.1, -1.2 };
    localization_observation_t observations[6];
    make_observations(SENSORS_3D, 6U, source, 12.345678, 237.0, observations);

    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 6U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_SOLVED);
    assert(!result.planar_layout);
    assert(result.combinations_total == 1U && result.combinations_valid == 1U);
    assert(distance(result.position, source) < 0.01);
    assert(fabs(result.velocity_mps - 237.0) < 1.0);
    assert(llabs(result.emission_time_us - 12345678) <= 20);
    assert(result.spread_m == 0.0);
    printf("[TDCAS] 6-sensor explicit: error=%.4f m v=%.2f m/s\n", distance(result.position, source), result.velocity_mps);
}


static void test_comprehensive_solution_with_pick_noise(void)
{
    const localization_point3d_t source = { 2.3, 1.9, -1.6 };
    localization_observation_t observations[8];
    make_observations(SENSORS_3D, 8U, source, 3.0, 310.0, observations);
    uint32_t seed = 12345U;
    for (size_t i = 0U; i < 8U; ++i) {
        observations[i].arrival_time_us += noise_us(&seed, 20.0);
    }

    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 8U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_SOLVED);
    assert(result.combinations_total == 28U);
    assert(result.combinations_valid >= 20U);
    const double error = distance(result.position, source);
    assert(error < 0.25);
    assert(result.spread_m > 0.0);
    assert(fabs(result.velocity_mps - 310.0) < 25.0);
    assert(result.rms_residual_us < 40.0);
    printf("[TDCAS] 8 sensors, +/-20us noise: error=%.4f m spread=%.3f m v=%.1f rms=%.1f us\n",
           error, result.spread_m, result.velocity_mps, result.rms_residual_us);
}


static void test_planar_surface_array_recovers_depth(void)
{
    const localization_point3d_t source = { 1.3, 2.2, -1.5 };
    localization_observation_t observations[8];
    make_observations(SENSORS_PLANAR, 8U, source, 0.5, 180.0, observations);

    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 5U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_SOLVED);
    assert(result.planar_layout);
    assert(result.combinations_total == 1U);
    assert(distance(result.position, source) < 0.03);
    assert(result.position.z_m < 0.0); /* buried, never the mirror image */

    assert(localization_tdcas_solve(observations, 8U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_SOLVED);
    assert(result.combinations_total == 56U);
    assert(distance(result.position, source) < 0.03);
    assert(fabs(result.velocity_mps - 180.0) < 2.0);
    printf("[TDCAS] planar 8-sensor surface array: error=%.4f m z=%.3f\n",
           distance(result.position, source), result.position.z_m);
}


/* 7 sensors flat (z = 0) plus one elevated sensor: the full 8-set resolves
 * depth (volume from the elevated sensor), but every 6-subset drawn purely
 * from the 7 flat sensors is exactly coplanar. The 3D solve path must
 * recognize that a coplanar 6-subset "cannot resolve z" (see the comment in
 * localization_tdcas_solve) and skip it without reading the subset's
 * would-be position -- regression for a stack read of an uninitialized
 * localization_point3d_t on that skip path. */
static const localization_point3d_t SENSORS_MOSTLY_FLAT[8] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.0 },
    { 0.0, 4.0, 0.0 },
    { 4.0, 4.0, 0.0 },
    { 2.0, 1.0, 0.0 },
    { 1.0, 3.0, 0.0 },
    { 3.0, 2.0, 0.0 },
    { 2.0, 2.0, 5.0 },
};


static void test_coplanar_subset_of_3d_array_is_skipped_not_uninitialized(void)
{
    const localization_point3d_t source = { 2.0, 2.0, -1.0 };
    localization_observation_t observations[8];
    make_observations(SENSORS_MOSTLY_FLAT, 8U, source, 0.25, 220.0, observations);

    /* The full 8-sensor set has volume (the elevated sensor), so it resolves
     * depth even though most of it is flat. */
    assert(localization_positions_resolve_depth(SENSORS_MOSTLY_FLAT, 8U));
    /* Every one of the 7 flat sensors is exactly coplanar with the others. */
    assert(!localization_positions_resolve_depth(SENSORS_MOSTLY_FLAT, 7U));

    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 8U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_SOLVED);
    assert(!result.planar_layout);
    /* C(8,6) = 28 combinations attempted; the C(7,6) = 7 combinations drawn
     * only from the flat sensors are coplanar and must be rejected before
     * ever touching the candidate position, leaving strictly fewer valid
     * combinations than attempted. */
    assert(result.combinations_total == 28U);
    assert(result.combinations_valid <= 21U);
    assert(result.combinations_valid > 0U);
    assert(distance(result.position, source) < 0.05);
    printf("[TDCAS] coplanar 6-subset skip: total=%u valid=%u error=%.4f m\n",
           (unsigned)result.combinations_total, (unsigned)result.combinations_valid,
           distance(result.position, source));
}


static void test_current_four_geophone_hardware_is_insufficient(void)
{
    const localization_point3d_t source = { 1.0, 1.0, -1.0 };
    localization_observation_t observations[8];
    make_observations(SENSORS_PLANAR, 8U, source, 0.0, 200.0, observations);

    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 4U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_INSUFFICIENT_SENSORS);

    /* Five sensors resolve a planar layout but not a 3D one. */
    make_observations(SENSORS_3D, 8U, source, 0.0, 200.0, observations);
    assert(localization_tdcas_solve(observations, 5U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_INSUFFICIENT_SENSORS);
    assert(!result.planar_layout);
}


static void test_velocity_free_beats_a_misspecified_velocity(void)
{
    const localization_point3d_t source = { 2.3, 1.9, -1.6 };
    const double true_velocity = 237.0;
    localization_observation_t observations[8];
    make_observations(SENSORS_3D, 8U, source, 1.0, true_velocity, observations);

    localization_tdcas_result_t tdcas;
    assert(localization_tdcas_solve(observations, 8U, NULL, &tdcas) == ESP_OK);
    assert(tdcas.status == LOCALIZATION_TDCAS_SOLVED);

    /* The Gauss-Newton TDOA solver needs a velocity; 15 % off is typical
     * for an uncalibrated rubble estimate. */
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = true_velocity * 1.15;
    config.max_iterations = 60;
    localization_result_t tdoa;
    assert(localization_tdoa_solve(observations, 8U, &config, NULL, &tdoa) == ESP_OK);

    const double tdcas_error = distance(tdcas.position, source);
    const double tdoa_error = distance(tdoa.source_position, source);
    assert(tdcas_error < 0.01);
    assert(tdoa_error > 10.0 * tdcas_error && tdoa_error > 0.05);
    printf("[TDCAS] velocity-free error=%.4f m vs TDOA with +15%% velocity error=%.4f m\n",
           tdcas_error, tdoa_error);
}


typedef struct
{
    localization_point3d_t obstructed_sensor;

} ray_context_t;


static bool ray_clear_unless_obstructed_sensor(
    const localization_point3d_t *sensor,
    const localization_point3d_t *candidate,
    void *context
)
{
    (void)candidate;
    const ray_context_t *ray = context;
    return distance(*sensor, ray->obstructed_sensor) > 1.0e-9;
}


static void test_scan_void_ray_filter_removes_detoured_sensor(void)
{
    /* Sensor 6's straight ray crosses a void: the wave detours and arrives
     * 1.5 ms late. Every combination containing it is biased. */
    const localization_point3d_t source = { 1.7, 1.1, -1.2 };
    localization_observation_t observations[7];
    make_observations(SENSORS_3D, 7U, source, 2.0, 237.0, observations);
    observations[6].arrival_time_us += 1500;

    localization_tdcas_result_t unfiltered;
    assert(localization_tdcas_solve(observations, 7U, NULL, &unfiltered) == ESP_OK);

    ray_context_t ray = { .obstructed_sensor = SENSORS_3D[6] };
    localization_tdcas_config_t config;
    memset(&config, 0, sizeof(config));
    config.ray_filter.clear = ray_clear_unless_obstructed_sensor;
    config.ray_filter.context = &ray;
    localization_tdcas_result_t filtered;
    assert(localization_tdcas_solve(observations, 7U, &config, &filtered) == ESP_OK);
    assert(filtered.status == LOCALIZATION_TDCAS_SOLVED);
    assert(filtered.combinations_total == 7U);
    /* Judged per sensor against the consensus, so sensor 6 is excluded
     * with all six combinations containing it. */
    assert(filtered.sensor_mask_obstructed == (1U << 6));
    assert(filtered.sensors_used == 6U);
    /* Only the one combination without sensor 6 is fused (some that
     * contain the delayed pick are not even physically valid). */
    assert(filtered.combinations_filtered == filtered.combinations_valid - 1U);

    const double unfiltered_error = unfiltered.status == LOCALIZATION_TDCAS_SOLVED
        ? distance(unfiltered.position, source)
        : INFINITY;
    const double filtered_error = distance(filtered.position, source);
    assert(filtered_error < 0.01);
    assert(unfiltered_error > 0.1);
    printf("[TDCAS] void-detoured pick: unfiltered error=%.3f m, ray-filtered error=%.4f m\n",
           unfiltered_error, filtered_error);
}


static bool above_one_meter_depth(const localization_point3d_t *candidate, void *context)
{
    (void)context;
    return candidate->z_m > -1.0;
}


static void test_candidate_filter_can_reject_every_combination(void)
{
    const localization_point3d_t source = { 1.7, 1.1, -1.2 };
    localization_observation_t observations[8];
    make_observations(SENSORS_3D, 8U, source, 2.0, 237.0, observations);

    localization_tdcas_config_t config;
    memset(&config, 0, sizeof(config));
    config.candidate_filter.allowed = above_one_meter_depth;
    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 8U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_NO_ALLOWED_COMBINATION);
    assert(result.combinations_filtered == result.combinations_valid);

    /* A zero filter admits everything. */
    memset(&config, 0, sizeof(config));
    assert(localization_tdcas_solve(observations, 8U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_SOLVED);
}


static void test_degenerate_and_invalid_input(void)
{
    localization_observation_t observations[9];
    for (size_t i = 0U; i < 8U; ++i) {
        observations[i].sensor_position = SENSORS_3D[i];
        observations[i].arrival_time_us = 1000; /* identical: no information */
    }
    observations[8] = observations[0];
    localization_tdcas_result_t result;
    assert(localization_tdcas_solve(observations, 8U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TDCAS_NO_VALID_COMBINATION);

    assert(localization_tdcas_solve(NULL, 6U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_tdcas_solve(observations, 6U, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_tdcas_solve(observations, 0U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_tdcas_solve(observations, 9U, NULL, &result) == ESP_ERR_INVALID_ARG);
    observations[2].sensor_position.y_m = NAN;
    assert(localization_tdcas_solve(observations, 6U, NULL, &result) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_six_sensor_explicit_solution();
    test_comprehensive_solution_with_pick_noise();
    test_planar_surface_array_recovers_depth();
    test_coplanar_subset_of_3d_array_is_skipped_not_uninitialized();
    test_current_four_geophone_hardware_is_insufficient();
    test_velocity_free_beats_a_misspecified_velocity();
    test_scan_void_ray_filter_removes_detoured_sensor();
    test_candidate_filter_can_reject_every_combination();
    test_degenerate_and_invalid_input();
    printf("localization TDCAS tests passed\n");
    return 0;
}
