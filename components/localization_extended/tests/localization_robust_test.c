#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ext_test_support.h"
#include "localization_robust.h"

/*
 * Robust arrival-time location (M-estimator + minimal-subset consensus):
 * one reflection picked as a first arrival must not drag the source, and
 * with too little redundancy the method must say it cannot tell which pick
 * is wrong instead of guessing.
 */

#define V_MPS 300.0
#define SIGMA_US 20.0

static const localization_point3d_t SOURCE = { 1.7, 1.4, -1.2 };


static localization_robust_config_t config_for(localization_extended_loss_t loss, bool consensus)
{
    localization_robust_config_t config = localization_robust_default_config();
    config.propagation_speed_mps = V_MPS;
    config.timing_uncertainty_us = SIGMA_US;
    config.loss = loss;
    config.consensus = consensus;
    return config;
}


/* Ordinary least squares: Huber with an unreachable threshold, no consensus. */
static localization_robust_config_t least_squares_config(void)
{
    localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_HUBER, false);
    config.tuning_k = 1.0e9;
    return config;
}


static void test_clean_picks_match_truth(void)
{
    uint32_t state = 11U;
    localization_observation_t observations[6];
    ext_test_observations(EXT_TEST_SENSORS_3D, 6U, SOURCE, 0.010, V_MPS, SIGMA_US, &state, observations);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    const localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);

    localization_robust_result_t result;
    assert(localization_robust_solve(observations, 6U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_ROBUST_SOLVED);
    assert(!result.planar);
    assert(result.consensus_run && !result.consensus_ambiguous);
    assert(result.subsets_evaluated == 15U); /* C(6, 4) */
    assert(result.outlier_mask == 0U && result.inlier_mask == 0x3FU);
    assert(result.observations_used == 6U && result.degrees_of_freedom == 2);
    assert(ext_test_distance(result.position, SOURCE) < 0.05);
    assert(fabs((double)result.emission_time_us - 10000.0) < 200.0);
    assert(result.rms_residual_us < 3.0 * SIGMA_US);
}


static void test_reflection_pick_is_identified_and_dropped(void)
{
    uint32_t state = 5U;
    localization_observation_t observations[6];
    ext_test_observations(EXT_TEST_SENSORS_3D, 6U, SOURCE, 0.010, V_MPS, SIGMA_US, &state, observations);
    observations[3].arrival_time_us += 4000; /* a reflection 4 ms late */
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();

    const localization_robust_config_t ls = least_squares_config();
    localization_robust_result_t plain;
    assert(localization_robust_solve(observations, 6U, &bounds, &ls, &plain) == ESP_OK);
    const double plain_error = ext_test_distance(plain.position, SOURCE);

    for (int loss = 0; loss < 2; ++loss) {
        const localization_robust_config_t config = config_for((localization_extended_loss_t)loss, true);
        localization_robust_result_t robust;
        assert(localization_robust_solve(observations, 6U, &bounds, &config, &robust) == ESP_OK);
        const double robust_error = ext_test_distance(robust.position, SOURCE);
        printf("  reflection pick: least squares %.3f m, robust (%s) %.3f m\n",
               plain_error, loss == 0 ? "Huber" : "Tukey", robust_error);
        assert(robust.status == LOCALIZATION_ROBUST_SOLVED);
        assert(!robust.consensus_ambiguous);
        assert(robust.outlier_mask == (1U << 3));
        assert(robust.observations_used == 5U && robust.degrees_of_freedom == 1);
        assert(robust.residual_us[3] > 2000.0);
        assert(robust_error < 0.06);
        assert(plain_error > 5.0 * robust_error && plain_error > 0.2);
    }
}


static void test_two_bad_picks_among_eight(void)
{
    uint32_t state = 21U;
    localization_observation_t observations[8];
    ext_test_observations(EXT_TEST_SENSORS_3D, 8U, SOURCE, 0.010, V_MPS, SIGMA_US, &state, observations);
    observations[1].arrival_time_us += 2500;
    observations[6].arrival_time_us -= 1800; /* an early noise trigger */
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    const localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);

    localization_robust_result_t result;
    assert(localization_robust_solve(observations, 8U, &bounds, &config, &result) == ESP_OK);
    assert(result.subsets_evaluated == 70U); /* C(8, 4) */
    assert(result.outlier_mask == ((1U << 1) | (1U << 6)));
    assert(result.observations_used == 6U);
    assert(ext_test_distance(result.position, SOURCE) < 0.05);
}


static void test_four_flat_pods_cannot_name_the_outlier(void)
{
    /* The current hardware in the planar (single layer) search: 3 unknowns
     * + t0 over 4 picks leaves one spare pick -- enough to see that
     * something is inconsistent, not enough to say what. */
    const localization_point3d_t source = { 1.1, 0.9, 0.0 };
    uint32_t state = 3U;
    localization_observation_t observations[4];
    ext_test_observations(EXT_TEST_SENSORS_FLAT, 4U, source, 0.0, V_MPS, 0.0, &state, observations);
    observations[2].arrival_time_us += 3000;
    const localization_extended_bounds_t bounds = { { -1.0, -1.0, 0.0 }, { 4.0, 3.5, 0.0 }, 0.05 };
    const localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);

    localization_robust_result_t result;
    assert(localization_robust_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_ROBUST_SOLVED);
    assert(result.planar);
    assert(result.consensus_run && result.subsets_evaluated == 4U);
    assert(result.consensus_ambiguous);
    assert(result.outlier_mask == 0U && result.observations_used == 4U);
    assert(result.degrees_of_freedom == 1);

    /* Clean picks: no ambiguity, the exact source. */
    ext_test_observations(EXT_TEST_SENSORS_FLAT, 4U, source, 0.0, V_MPS, 0.0, &state, observations);
    assert(localization_robust_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(!result.consensus_ambiguous && result.outlier_mask == 0U);
    assert(ext_test_distance(result.position, source) < 0.01);
}


static void test_exactly_determined_3d_has_no_consensus(void)
{
    uint32_t state = 8U;
    localization_observation_t observations[4];
    ext_test_observations(EXT_TEST_SENSORS_3D, 4U, SOURCE, 0.0, V_MPS, 0.0, &state, observations);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    const localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);
    localization_robust_result_t result;
    assert(localization_robust_solve(observations, 4U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_ROBUST_SOLVED);
    assert(!result.consensus_run);
    assert(result.degrees_of_freedom == 0);
    assert(ext_test_distance(result.position, SOURCE) < 0.02);

    assert(localization_robust_solve(observations, 3U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_ROBUST_INSUFFICIENT_OBSERVATIONS);
}


static void test_m_estimator_alone_needs_redundancy(void)
{
    /* Without the consensus the M-estimator starts from the least-squares
     * neighbourhood. With 2 spare picks the fit absorbs a 4 ms outlier by
     * moving ~1 m (its residual is masked) and no reweighting can undo
     * that -- which is why the consensus runs by default. With 4 spare
     * picks the outlier stands out and the redescending loss drops it. */
    uint32_t state = 5U;
    localization_observation_t observations[8];
    ext_test_observations(EXT_TEST_SENSORS_3D, 8U, SOURCE, 0.010, V_MPS, SIGMA_US, &state, observations);
    observations[3].arrival_time_us += 4000;
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    const localization_robust_config_t ls = least_squares_config();
    const localization_robust_config_t tukey = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, false);
    localization_robust_result_t result;

    assert(localization_robust_solve(observations, 6U, &bounds, &ls, &result) == ESP_OK);
    const double ls_six = ext_test_distance(result.position, SOURCE);
    assert(localization_robust_solve(observations, 6U, &bounds, &tukey, &result) == ESP_OK);
    const double tukey_six = ext_test_distance(result.position, SOURCE);
    assert(!result.consensus_run && result.outlier_mask == 0U);

    assert(localization_robust_solve(observations, 8U, &bounds, &ls, &result) == ESP_OK);
    const double ls_eight = ext_test_distance(result.position, SOURCE);
    assert(localization_robust_solve(observations, 8U, &bounds, &tukey, &result) == ESP_OK);
    const double tukey_eight = ext_test_distance(result.position, SOURCE);
    printf("  M-estimator alone: 6 picks LS %.3f / Tukey %.3f m; 8 picks LS %.3f / Tukey %.3f m\n",
           ls_six, tukey_six, ls_eight, tukey_eight);
    assert(tukey_six > 0.3);
    assert(tukey_eight < 0.06 && ls_eight > 5.0 * tukey_eight);
    assert(fabs(result.residual_us[3] - 4000.0) < 300.0);
}


static bool below_minus_half(const localization_point3d_t *candidate, void *context)
{
    (void)context;
    return candidate->z_m <= -0.5;
}


static void test_candidate_filter_is_respected(void)
{
    uint32_t state = 2U;
    localization_observation_t observations[6];
    ext_test_observations(EXT_TEST_SENSORS_3D, 6U, SOURCE, 0.0, V_MPS, SIGMA_US, &state, observations);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);
    config.candidate_filter.allowed = below_minus_half;
    localization_robust_result_t result;
    assert(localization_robust_solve(observations, 6U, &bounds, &config, &result) == ESP_OK);
    assert(result.position.z_m <= -0.5);
    assert(ext_test_distance(result.position, SOURCE) < 0.05);

    const localization_extended_bounds_t shallow = { { -1.0, -1.0, -0.4 }, { 5.0, 4.5, 0.8 }, 0.1 };
    assert(localization_robust_solve(observations, 6U, &shallow, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_ROBUST_NO_ALLOWED_CANDIDATE);
}


static void test_invalid_input(void)
{
    localization_observation_t observations[4];
    uint32_t state = 1U;
    ext_test_observations(EXT_TEST_SENSORS_3D, 4U, SOURCE, 0.0, V_MPS, 0.0, &state, observations);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_robust_config_t config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);
    localization_robust_result_t result;
    assert(localization_robust_solve(NULL, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_robust_solve(observations, 0U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_robust_solve(observations, 9U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_robust_solve(observations, 4U, NULL, &config, &result) == ESP_ERR_INVALID_ARG);
    config.propagation_speed_mps = 0.0;
    assert(localization_robust_solve(observations, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);
    config.timing_uncertainty_us = NAN;
    assert(localization_robust_solve(observations, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = config_for(LOCALIZATION_EXTENDED_LOSS_TUKEY, true);
    localization_extended_bounds_t inverted = bounds;
    inverted.min_corner.x_m = 9.0;
    assert(localization_robust_solve(observations, 4U, &inverted, &config, &result) == ESP_ERR_INVALID_ARG);
}


static void test_loss_functions(void)
{
    const double k = LOCALIZATION_EXTENDED_HUBER_K;
    assert(localization_extended_robust_weight(LOCALIZATION_EXTENDED_LOSS_HUBER, 0.5, k) == 1.0);
    assert(fabs(localization_extended_robust_weight(LOCALIZATION_EXTENDED_LOSS_HUBER, 4.0 * k, k) - 0.25) < 1e-12);
    assert(fabs(localization_extended_robust_rho(LOCALIZATION_EXTENDED_LOSS_HUBER, 2.0 * k, k) - 1.5 * k * k) < 1e-12);
    const double t = LOCALIZATION_EXTENDED_TUKEY_K;
    assert(localization_extended_robust_weight(LOCALIZATION_EXTENDED_LOSS_TUKEY, 0.0, t) == 1.0);
    assert(localization_extended_robust_weight(LOCALIZATION_EXTENDED_LOSS_TUKEY, t, t) == 0.0);
    assert(fabs(localization_extended_robust_rho(LOCALIZATION_EXTENDED_LOSS_TUKEY, 10.0 * t, t) - t * t / 6.0) < 1e-12);
    /* Continuous at the threshold, and bounded: an outlier's influence ends. */
    assert(fabs(localization_extended_robust_rho(LOCALIZATION_EXTENDED_LOSS_TUKEY, t - 1e-9, t) - t * t / 6.0) < 1e-6);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_loss_functions();
    test_clean_picks_match_truth();
    test_reflection_pick_is_identified_and_dropped();
    test_two_bad_picks_among_eight();
    test_four_flat_pods_cannot_name_the_outlier();
    test_exactly_determined_3d_has_no_consensus();
    test_m_estimator_alone_needs_redundancy();
    test_candidate_filter_is_respected();
    test_invalid_input();
    printf("localization robust tests passed\n");
    return 0;
}
