#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization.h"
#include "localization_psafuvs.h"

/*
 * PSAFUVS (Dong & Li 2013): P and S arrivals, no pre-measured velocity.
 * Includes the 4-sensor flat layout of this project's current hardware.
 */

static double distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


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
    double vp,
    double vs,
    localization_psafuvs_observation_t *out
)
{
    for (size_t i = 0U; i < count; ++i) {
        const double d = distance(source, sensors[i]);
        out[i].sensor_position = sensors[i];
        out[i].p_arrival_time_us = (int64_t)llround((emission_s + d / vp) * 1.0e6);
        out[i].s_arrival_time_us = (int64_t)llround((emission_s + d / vs) * 1.0e6);
    }
}


/* The current hardware: four geophone pods on a flat rubble surface. */
static const localization_point3d_t FOUR_PODS[4] = {
    { 0.0, 0.0, 0.0 },
    { 3.0, 0.2, 0.0 },
    { 0.3, 2.8, 0.0 },
    { 3.2, 3.1, 0.0 },
};

static const localization_point3d_t SENSORS_3D[6] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.3 },
    { 0.0, 3.5, 0.6 },
    { 4.0, 3.5, 1.4 },
    { 2.0, 1.5, 2.2 },
    { 1.0, 2.8, -0.9 },
};


static void test_four_flat_geophones_locate_buried_source(void)
{
    const localization_point3d_t source = { 1.2, 1.9, -1.4 };
    localization_psafuvs_observation_t observations[4];
    make_observations(FOUR_PODS, 4U, source, 7.25, 320.0, 170.0, observations);

    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 4U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(result.planar_layout);
    assert(result.sensors_used == 4U && result.sensor_mask_used == 0xFU);
    assert(distance(result.position, source) < 0.02);
    assert(result.position.z_m < 0.0);
    assert(fabs(result.p_velocity_mps - 320.0) < 3.0);
    assert(fabs(result.s_velocity_mps - 170.0) < 2.0);
    assert(fabs(result.virtual_velocity_mps - 320.0 * 170.0 / 150.0) < 10.0);
    assert(llabs(result.emission_time_us - 7250000) <= 50);
    printf("[PSAFUVS] 4 flat pods: error=%.4f m vP=%.1f vS=%.1f rms=%.2f us iterations=%u\n",
           distance(result.position, source), result.p_velocity_mps, result.s_velocity_mps,
           result.rms_residual_us, (unsigned)result.iterations);
}


static void test_depth_resolving_layout(void)
{
    const localization_point3d_t source = { 2.2, 1.4, -1.1 };
    localization_psafuvs_observation_t observations[6];
    make_observations(SENSORS_3D, 5U, source, 1.0, 410.0, 230.0, observations);

    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 5U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(!result.planar_layout);
    assert(distance(result.position, source) < 0.02);
    assert(fabs(result.p_velocity_mps - 410.0) < 4.0);
    assert(fabs(result.s_velocity_mps - 230.0) < 3.0);

    /* Four depth-resolving sensors (a tetrahedron) are one short in 3D. */
    const localization_point3d_t tetrahedron[4] = {
        SENSORS_3D[0], SENSORS_3D[1], SENSORS_3D[2], SENSORS_3D[4],
    };
    assert(localization_positions_resolve_depth(tetrahedron, 4U));
    make_observations(tetrahedron, 4U, source, 1.0, 410.0, 230.0, observations);
    assert(localization_psafuvs_solve(observations, 4U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS);
    assert(!result.planar_layout);
}


static void test_tilted_surface_places_source_below_the_slope(void)
{
    /* The four pods on a 25-degree rubble slope. */
    const double tilt = 25.0 * 3.14159265358979323846 / 180.0;
    localization_point3d_t sensors[4];
    for (size_t i = 0U; i < 4U; ++i) {
        sensors[i] = (localization_point3d_t) {
            FOUR_PODS[i].x_m,
            FOUR_PODS[i].y_m * cos(tilt),
            FOUR_PODS[i].y_m * sin(tilt) + 2.0,
        };
    }
    /* 1.2 m beneath the slope along its normal. */
    const localization_point3d_t on_slope = { 1.5, 1.5 * cos(tilt), 1.5 * sin(tilt) + 2.0 };
    const localization_point3d_t source = {
        on_slope.x_m, on_slope.y_m + 1.2 * sin(tilt), on_slope.z_m - 1.2 * cos(tilt),
    };
    localization_psafuvs_observation_t observations[4];
    make_observations(sensors, 4U, source, 0.0, 300.0, 160.0, observations);

    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 4U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(result.planar_layout);
    assert(distance(result.position, source) < 0.03);
}


static void test_pick_noise_is_bounded(void)
{
    const localization_point3d_t source = { 1.2, 1.9, -1.4 };
    localization_psafuvs_observation_t observations[4];
    make_observations(FOUR_PODS, 4U, source, 0.0, 320.0, 170.0, observations);
    uint32_t seed = 99U;
    for (size_t i = 0U; i < 4U; ++i) {
        observations[i].p_arrival_time_us += noise_us(&seed, 20.0);
        observations[i].s_arrival_time_us += noise_us(&seed, 40.0); /* S picks are coarser */
    }
    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 4U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    const double error = distance(result.position, source);
    assert(error < 0.35);
    assert(result.rms_residual_us < 40.0);
    printf("[PSAFUVS] 4 pods with +/-20us P / +/-40us S noise: error=%.3f m rms=%.1f us\n",
           error, result.rms_residual_us);
}


static void test_velocity_free_beats_a_misspecified_velocity(void)
{
    /* Four flat sensors cannot even run the 3D TDOA solver (coplanar);
     * with a 5-sensor 3D layout TDOA runs but needs the velocity. */
    const localization_point3d_t source = { 2.2, 1.4, -1.1 };
    localization_psafuvs_observation_t observations[6];
    make_observations(SENSORS_3D, 6U, source, 1.0, 410.0, 230.0, observations);

    localization_psafuvs_result_t psafuvs;
    assert(localization_psafuvs_solve(observations, 6U, NULL, &psafuvs) == ESP_OK);
    assert(psafuvs.status == LOCALIZATION_PSAFUVS_SOLVED);

    localization_observation_t p_only[6];
    for (size_t i = 0U; i < 6U; ++i) {
        p_only[i].sensor_position = observations[i].sensor_position;
        p_only[i].arrival_time_us = observations[i].p_arrival_time_us;
    }
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = 410.0 * 0.85;
    config.max_iterations = 60;
    localization_result_t tdoa;
    assert(localization_tdoa_solve(p_only, 6U, &config, NULL, &tdoa) == ESP_OK);
    const double psafuvs_error = distance(psafuvs.position, source);
    const double tdoa_error = distance(tdoa.source_position, source);
    assert(psafuvs_error < 0.01 && tdoa_error > 0.05);
    printf("[PSAFUVS] velocity-free error=%.4f m vs TDOA with -15%% velocity error=%.3f m\n",
           psafuvs_error, tdoa_error);
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


static void test_scan_void_ray_filter_drops_detoured_sensor(void)
{
    const localization_point3d_t source = { 2.2, 1.4, -1.1 };
    localization_psafuvs_observation_t observations[6];
    make_observations(SENSORS_3D, 6U, source, 1.0, 410.0, 230.0, observations);
    /* Sensor 3's ray detours around a void: both phases arrive late. */
    observations[3].p_arrival_time_us += 1200;
    observations[3].s_arrival_time_us += 2100;

    localization_psafuvs_result_t unfiltered;
    assert(localization_psafuvs_solve(observations, 6U, NULL, &unfiltered) == ESP_OK);

    ray_context_t ray = { .obstructed_sensor = SENSORS_3D[3] };
    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    config.ray_filter.clear = ray_clear_unless_obstructed_sensor;
    config.ray_filter.context = &ray;
    localization_psafuvs_result_t filtered;
    assert(localization_psafuvs_solve(observations, 6U, &config, &filtered) == ESP_OK);
    assert(filtered.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(filtered.sensor_mask_obstructed == (1U << 3));
    assert(filtered.sensor_mask_used == (0x3FU & ~(1U << 3)));
    assert(filtered.sensors_used == 5U);

    const double unfiltered_error = unfiltered.status == LOCALIZATION_PSAFUVS_SOLVED
        ? distance(unfiltered.position, source)
        : INFINITY;
    const double filtered_error = distance(filtered.position, source);
    assert(filtered_error < 0.02);
    assert(unfiltered_error > 0.1);
    printf("[PSAFUVS] void-detoured sensor: unfiltered error=%.3f m, ray-filtered error=%.4f m\n",
           unfiltered_error, filtered_error);

    /* With only the minimum sensors, dropping one leaves too few. */
    make_observations(SENSORS_3D, 5U, source, 1.0, 410.0, 230.0, observations);
    assert(localization_psafuvs_solve(observations, 5U, &config, &filtered) == ESP_OK);
    assert(filtered.status == LOCALIZATION_PSAFUVS_RAYS_OBSTRUCTED);
}


static bool reject_everything(const localization_point3d_t *candidate, void *context)
{
    (void)candidate;
    (void)context;
    return false;
}


static void test_candidate_filter_and_invalid_input(void)
{
    const localization_point3d_t source = { 1.2, 1.9, -1.4 };
    localization_psafuvs_observation_t observations[4];
    make_observations(FOUR_PODS, 4U, source, 0.0, 320.0, 170.0, observations);

    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    config.candidate_filter.allowed = reject_everything;
    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 4U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_NO_ALLOWED_CANDIDATE);

    assert(localization_psafuvs_solve(observations, 3U, NULL, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS);

    /* No max_iterations: the analytical solution alone is already exact. */
    config = localization_psafuvs_default_config();
    config.max_iterations = 0U;
    assert(localization_psafuvs_solve(observations, 4U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(distance(result.position, source) < 0.02);

    /* S must follow P. */
    observations[1].s_arrival_time_us = observations[1].p_arrival_time_us;
    assert(localization_psafuvs_solve(observations, 4U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_psafuvs_solve(NULL, 4U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_psafuvs_solve(observations, 9U, NULL, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_psafuvs_solve(observations, 4U, NULL, NULL) == ESP_ERR_INVALID_ARG);
}


static void test_inconsistent_arrivals_are_not_solved(void)
{
    /* Identical S-P delays at every sensor but differing P times: no
     * single source/velocity pair explains them. */
    localization_psafuvs_observation_t observations[4];
    for (size_t i = 0U; i < 4U; ++i) {
        observations[i].sensor_position = FOUR_PODS[i];
        observations[i].p_arrival_time_us = 1000 + (int64_t)(i * 3000U);
        observations[i].s_arrival_time_us = observations[i].p_arrival_time_us + 2000;
    }
    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 4U, NULL, &result) == ESP_OK);
    assert(result.status != LOCALIZATION_PSAFUVS_SOLVED || result.rms_residual_us > 500.0);
}


static void test_four_pods_on_one_circle_are_degenerate(void)
{
    /* With four coplanar sensors the affine identity
     * sum a_i |x - s_i|^2 = sum a_i |s_i|^2 is what fixes the scale K;
     * its right-hand side vanishes exactly for concyclic sensors (a
     * rectangle -- the British flag theorem -- or an isosceles trapezoid),
     * and then every K fits: the solver must not return a location. */
    static const localization_point3d_t rectangle[4] = {
        { 0.0, 0.0, 0.0 }, { 3.0, 0.0, 0.0 }, { 0.0, 2.5, 0.0 }, { 3.0, 2.5, 0.0 },
    };
    static const localization_point3d_t trapezoid[4] = {
        { 0.0, 0.0, 0.0 }, { 3.0, 0.0, 0.0 }, { 0.6, 2.5, 0.0 }, { 2.4, 2.5, 0.0 },
    };
    const localization_point3d_t source = { 1.1, 1.6, -1.3 };
    localization_psafuvs_observation_t observations[4];
    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    localization_psafuvs_result_t result;

    make_observations(rectangle, 4U, source, 0.02, 320.0, 180.0, observations);
    assert(localization_psafuvs_solve(observations, 4U, &config, &result) == ESP_OK);
    assert(result.status != LOCALIZATION_PSAFUVS_SOLVED);
    make_observations(trapezoid, 4U, source, 0.02, 320.0, 180.0, observations);
    assert(localization_psafuvs_solve(observations, 4U, &config, &result) == ESP_OK);
    assert(result.status != LOCALIZATION_PSAFUVS_SOLVED);
    /* FOUR_PODS is off any circle: solved (test above). */
}


static void test_velocity_bounds_and_unset_compatibility(void)
{
    const localization_point3d_t source = { 1.2, 1.9, -1.4 };
    localization_psafuvs_observation_t observations[4];
    make_observations(FOUR_PODS, 4U, source, 7.25, 320.0, 170.0, observations);

    localization_psafuvs_result_t legacy;
    localization_psafuvs_result_t unset;
    assert(localization_psafuvs_solve(observations, 4U, NULL, &legacy) == ESP_OK);
    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    localization_velocity_bounds_t bounds = { 0 };
    config.velocity_bounds = &bounds;
    assert(localization_psafuvs_solve(observations, 4U, &config, &unset) == ESP_OK);
    /* Compatibility: unset bounds retain the pre-range result bytes. */
    assert(memcmp(&legacy, &unset, sizeof(legacy)) == 0);

    bounds = (localization_velocity_bounds_t) {
        .v_min_mps = 316.8f, .v_max_mps = 323.2f,
        .vp_vs_min = 1.80f, .vp_vs_max = 1.95f, .set = true,
    };
    config.velocity_bounds = &bounds;
    config.timing_uncertainty_us = 20.0;
    localization_psafuvs_result_t narrow;
    assert(localization_psafuvs_solve(observations, 4U, &config, &narrow) == ESP_OK);
    assert(narrow.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(narrow.velocity_status == LOCALIZATION_VELOCITY_STATUS_OK ||
           narrow.velocity_status == LOCALIZATION_VELOCITY_STATUS_AT_BOUND);
    assert(distance(narrow.position, legacy.position) < 0.03);

    bounds.v_min_mps = 300.0f;
    bounds.v_max_mps = 400.0f;
    bounds.vp_vs_min = 1.5f;
    bounds.vp_vs_max = 2.5f;
    localization_psafuvs_result_t wide;
    assert(localization_psafuvs_solve(observations, 4U, &config, &wide) == ESP_OK);
    assert(wide.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(wide.velocity_status == LOCALIZATION_VELOCITY_STATUS_OK);
    assert(distance(wide.position, source) < 0.03);

    bounds.v_min_mps = 320.0f;
    bounds.v_max_mps = 320.0f;
    bounds.vp_vs_min = 320.0f / 170.0f;
    bounds.vp_vs_max = bounds.vp_vs_min;
    localization_psafuvs_result_t fixed;
    assert(localization_psafuvs_solve(observations, 4U, &config, &fixed) == ESP_OK);
    assert(fixed.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(fixed.velocity_status == LOCALIZATION_VELOCITY_STATUS_OK);
    assert(fabs(fixed.p_velocity_mps - 320.0) < 1.0e-9);
    assert(fabs(fixed.s_velocity_mps - 170.0) < 1.0e-6);

    make_observations(FOUR_PODS, 4U, source, 7.25, 600.0, 300.0, observations);
    localization_psafuvs_result_t fixed_mismatch;
    assert(localization_psafuvs_solve(observations, 4U, &config, &fixed_mismatch) == ESP_OK);
    assert(fixed_mismatch.velocity_status == LOCALIZATION_VELOCITY_STATUS_INCONSISTENT);

    make_observations(FOUR_PODS, 4U, source, 7.25, 320.0, 170.0, observations);
    bounds.v_min_mps = 500.0f;
    bounds.v_max_mps = 600.0f;
    localization_psafuvs_result_t outside;
    assert(localization_psafuvs_solve(observations, 4U, &config, &outside) == ESP_OK);
    assert(outside.velocity_status == LOCALIZATION_VELOCITY_STATUS_AT_BOUND ||
           outside.velocity_status == LOCALIZATION_VELOCITY_STATUS_INCONSISTENT);
    assert(outside.velocity_status != LOCALIZATION_VELOCITY_STATUS_OK);

    /* One badly mislabeled S arrival cannot be presented as an OK range fit. */
    make_observations(FOUR_PODS, 4U, source, 7.25, 320.0, 170.0, observations);
    observations[2].s_arrival_time_us += 100000;
    bounds.v_min_mps = 300.0f;
    bounds.v_max_mps = 400.0f;
    localization_psafuvs_result_t mislabeled;
    assert(localization_psafuvs_solve(observations, 4U, &config, &mislabeled) == ESP_OK);
    assert(mislabeled.velocity_status == LOCALIZATION_VELOCITY_STATUS_AT_BOUND ||
           mislabeled.velocity_status == LOCALIZATION_VELOCITY_STATUS_INCONSISTENT);
}


static void test_bounded_fit_converges_at_projected_optimum(void)
{
    const localization_point3d_t source = { 1.2, 1.9, -1.4 };
    localization_psafuvs_observation_t observations[4];
    make_observations(FOUR_PODS, 4U, source, 7.25, 400.0, 200.0, observations);
    const int64_t noise_p_us[4] = { 900, -700, 500, -300 };
    const int64_t noise_s_us[4] = { -800, 600, -400, 1000 };
    for (size_t i = 0U; i < 4U; ++i) {
        observations[i].p_arrival_time_us += noise_p_us[i];
        observations[i].s_arrival_time_us += noise_s_us[i];
    }
    const localization_velocity_bounds_t bounds = {
        .v_min_mps = 310.0f, .v_max_mps = 315.0f, .set = true,
    };
    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    config.max_iterations = 30U;
    config.timing_uncertainty_us = 10000.0;
    config.velocity_bounds = &bounds;
    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 4U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(result.converged);
    assert(result.velocity_status == LOCALIZATION_VELOCITY_STATUS_AT_BOUND);
    assert(fabs(result.p_velocity_mps - 315.0) < 1.0e-3);
    assert(result.iterations < config.max_iterations);
}


int main(void)
{
    test_four_flat_geophones_locate_buried_source();
    test_depth_resolving_layout();
    test_tilted_surface_places_source_below_the_slope();
    test_pick_noise_is_bounded();
    test_velocity_free_beats_a_misspecified_velocity();
    test_scan_void_ray_filter_drops_detoured_sensor();
    test_candidate_filter_and_invalid_input();
    test_inconsistent_arrivals_are_not_solved();
    test_four_pods_on_one_circle_are_degenerate();
    test_velocity_bounds_and_unset_compatibility();
    test_bounded_fit_converges_at_projected_optimum();
    printf("localization PSAFUVS tests passed\n");
    return 0;
}
