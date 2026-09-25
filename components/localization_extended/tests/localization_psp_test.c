#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext_test_support.h"
#include "localization_psp.h"

/*
 * PSP (joint P-TDOA + P-S): on the current four flat pods the P-S
 * intervals supply the range that P differences alone cannot, so the fit
 * is over-determined in 3D; a doubtful S pick given a large sigma must
 * barely move the result.
 */

#define VP 320.0
#define VS 180.0
#define SIGMA_US 20.0

static const localization_point3d_t SOURCE = { 1.1, 1.6, -1.3 };


static localization_extended_bounds_t flat_bounds(void)
{
    const localization_extended_bounds_t bounds = { { -1.0, -1.0, -3.0 }, { 4.0, 3.5, 0.0 }, 0.1 };
    return bounds;
}


static void make(
    const localization_point3d_t *sensors,
    size_t count,
    localization_point3d_t source,
    double noise_us,
    uint32_t *state,
    localization_psp_observation_t *out
)
{
    for (size_t i = 0U; i < count; ++i) {
        const double d = ext_test_distance(source, sensors[i]);
        const double np = noise_us > 0.0 ? noise_us * ext_test_normal(state) : 0.0;
        const double ns = noise_us > 0.0 ? noise_us * ext_test_normal(state) : 0.0;
        out[i].sensor_position = sensors[i];
        out[i].p_arrival_time_us = (int64_t)llround((0.02 + d / VP) * 1.0e6 + np);
        out[i].s_valid = true;
        out[i].s_arrival_time_us = (int64_t)llround((0.02 + d / VS) * 1.0e6 + ns);
        out[i].s_minus_p_sigma_us = 0.0;
    }
}


static localization_psp_config_t free_config(void)
{
    localization_psp_config_t config;
    memset(&config, 0, sizeof(config));
    config.timing_uncertainty_us = SIGMA_US;
    config.p_velocity_sigma_mps = -1.0; /* vP unknown */
    return config;
}


static localization_psp_config_t prior_config(double sigma_mps)
{
    localization_psp_config_t config = free_config();
    config.p_velocity_mps = VP;
    config.p_velocity_sigma_mps = sigma_mps;
    return config;
}


static void test_four_flat_pods_need_a_velocity_prior(void)
{
    /* Four coplanar sensors with BOTH velocities unknown: the S-P
     * intervals fix only distance ratios, and the P picks then only fix
     * p/q -- a one-parameter family (depth against velocity) fits exactly.
     * The solver must say so rather than return one arbitrary member. */
    uint32_t state = 4U;
    localization_psp_observation_t observations[4];
    make(EXT_TEST_SENSORS_FLAT, 4U, SOURCE, 0.0, &state, observations);
    const localization_extended_bounds_t bounds = flat_bounds();

    localization_psp_config_t config = free_config();
    localization_psp_result_t result;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    printf("  4 flat pods, vP free: rcond %.1e -> underdetermined\n", result.normal_rcond);
    assert(result.status == LOCALIZATION_PSP_UNDERDETERMINED);
    assert(result.degrees_of_freedom == 2); /* counting rows is not enough */

    /* The caller's calibrated vP as a prior (+-5 m/s) resolves it: depth
     * and vS come out of the S-P intervals. */
    config = prior_config(5.0);
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    printf("  4 flat pods, vP prior: rcond %.1e, error %.4f m, vS %.1f m/s\n",
           result.normal_rcond, ext_test_distance(result.position, SOURCE), result.s_velocity_mps);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(!result.planar);
    assert(result.p_used == 4U && result.s_used == 4U && result.s_mask_used == 0xFU);
    /* 8 rows + prior, 6 unknowns (x, y, z, t0, p, q). */
    assert(result.degrees_of_freedom == 3);
    assert(ext_test_distance(result.position, SOURCE) < 0.01);
    assert(fabs(result.p_velocity_mps - VP) < 1.0);
    assert(fabs(result.s_velocity_mps - VS) < 1.0);
    assert(llabs(result.emission_time_us - 20000) <= 5);

    /* With 20 us pick noise (~6 mm of travel) the depth, set by S-P
     * ratios over a 3 m array, is good to a couple of decimetres. */
    make(EXT_TEST_SENSORS_FLAT, 4U, SOURCE, SIGMA_US, &state, observations);
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    printf("  4 flat pods, 20 us noise: error %.3f m, vP %.1f, vS %.1f m/s\n",
           ext_test_distance(result.position, SOURCE), result.p_velocity_mps, result.s_velocity_mps);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(ext_test_distance(result.position, SOURCE) < 0.3);

    /* A depth-resolving layout needs no prior at all. */
    localization_psp_observation_t relief[6];
    make(EXT_TEST_SENSORS_3D, 6U, SOURCE, 0.0, &state, relief);
    const localization_extended_bounds_t box = ext_test_bounds_3d();
    config = free_config();
    assert(localization_psp_solve(relief, 6U, &box, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(ext_test_distance(result.position, SOURCE) < 0.01);
    assert(fabs(result.p_velocity_mps - VP) < 1.0 && fabs(result.s_velocity_mps - VS) < 1.0);
}


static void test_missing_s_picks_degrade_gracefully(void)
{
    uint32_t state = 9U;
    localization_psp_observation_t observations[4];
    make(EXT_TEST_SENSORS_FLAT, 4U, SOURCE, 0.0, &state, observations);
    observations[2].s_valid = false;
    const localization_extended_bounds_t bounds = flat_bounds();
    localization_psp_config_t config = prior_config(5.0);
    localization_psp_result_t result;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(result.s_used == 3U && result.s_mask_used == 0xBU);
    assert(result.degrees_of_freedom == 2);
    assert(ext_test_distance(result.position, SOURCE) < 0.01);

    /* A single S pick with vP known: 5 rows for x, y, z, t0, q. */
    for (size_t i = 1U; i < 4U; ++i) {
        observations[i].s_valid = false;
    }
    config.p_velocity_sigma_mps = 0.0;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.degrees_of_freedom == 0);
    assert(fabs(result.p_velocity_mps - VP) < 1e-9);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    /* Exactly determined: the 1 us rounding of the picks shows. */
    assert(ext_test_distance(result.position, SOURCE) < 0.05);

    for (size_t i = 0U; i < 4U; ++i) {
        observations[i].s_valid = false;
    }
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_NO_S_PICK);
}


static void test_doubtful_s_pick_is_down_weighted(void)
{
    const localization_point3d_t *sensors = EXT_TEST_SENSORS_3D;
    uint32_t state = 17U;
    localization_psp_observation_t observations[6];
    make(sensors, 6U, SOURCE, SIGMA_US, &state, observations);
    /* A converted/reflected phase taken as S, 3 ms late. */
    observations[4].s_arrival_time_us += 3000;
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_psp_config_t config = free_config();

    localization_psp_result_t equal;
    assert(localization_psp_solve(observations, 6U, &bounds, &config, &equal) == ESP_OK);
    /* Its picker reported a poor SNR: sigma 20x a good S pick. */
    observations[4].s_minus_p_sigma_us = 20.0 * sqrt(2.0) * SIGMA_US;
    localization_psp_result_t weighted;
    assert(localization_psp_solve(observations, 6U, &bounds, &config, &weighted) == ESP_OK);
    const double equal_error = ext_test_distance(equal.position, SOURCE);
    const double weighted_error = ext_test_distance(weighted.position, SOURCE);
    printf("  bad S pick: equal weights %.3f m, confidence-weighted %.3f m\n", equal_error, weighted_error);
    assert(weighted.status == LOCALIZATION_PSP_SOLVED);
    assert(weighted.s_used == 6U); /* kept, not thrown away */
    assert(weighted_error < 0.08);
    assert(equal_error > 3.0 * weighted_error);
    assert(fabs(weighted.s_velocity_mps - VS) < 5.0);
}


static void test_velocity_prior(void)
{
    uint32_t state = 23U;
    localization_psp_observation_t observations[4];
    make(EXT_TEST_SENSORS_FLAT, 4U, SOURCE, SIGMA_US, &state, observations);
    const localization_extended_bounds_t bounds = flat_bounds();
    localization_psp_config_t config = free_config();
    config.p_velocity_mps = VP;
    config.p_velocity_sigma_mps = 5.0;
    localization_psp_result_t result;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(result.degrees_of_freedom == 3); /* the prior is one more row */
    assert(fabs(result.p_velocity_mps - VP) < 10.0);
    assert(ext_test_distance(result.position, SOURCE) < 0.3);
}


static bool never(const localization_point3d_t *candidate, void *context)
{
    (void)candidate;
    (void)context;
    return false;
}


static void test_invalid_and_rejected(void)
{
    uint32_t state = 1U;
    localization_psp_observation_t observations[4];
    make(EXT_TEST_SENSORS_FLAT, 4U, SOURCE, 0.0, &state, observations);
    const localization_extended_bounds_t bounds = flat_bounds();
    localization_psp_config_t config = free_config();
    localization_psp_result_t result;

    config.candidate_filter.allowed = never;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_NO_ALLOWED_CANDIDATE);
    config = free_config();

    /* Planar search at the source depth: (x, y, t0, p, q) over 8 rows. */
    const localization_extended_bounds_t plane = { { -1.0, -1.0, -1.3 }, { 4.0, 3.5, -1.3 }, 0.05 };
    assert(localization_psp_solve(observations, 4U, &plane, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(result.planar && result.degrees_of_freedom == 3);
    assert(ext_test_distance(result.position, SOURCE) < 0.01);

    /* Three sensors, vP free: 6 rows for 6 unknowns -- counted, but not
     * identifiable. */
    assert(localization_psp_solve(observations, 3U, &bounds, &config, &result) == ESP_OK);
    assert(result.degrees_of_freedom == 0);
    assert(result.status == LOCALIZATION_PSP_UNDERDETERMINED);
    /* Two: 4 rows for 6 unknowns. */
    assert(localization_psp_solve(observations, 2U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_INSUFFICIENT_OBSERVATIONS);

    assert(localization_psp_solve(NULL, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_psp_solve(observations, 9U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config.timing_uncertainty_us = 0.0;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = free_config();
    config.p_velocity_sigma_mps = 0.0; /* fixed but no velocity given */
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = free_config();
    observations[1].s_arrival_time_us = observations[1].p_arrival_time_us;
    assert(localization_psp_solve(observations, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_four_flat_pods_need_a_velocity_prior();
    test_missing_s_picks_degrade_gracefully();
    test_doubtful_s_pick_is_down_weighted();
    test_velocity_prior();
    test_invalid_and_rejected();
    printf("localization PSP tests passed\n");
    return 0;
}
