#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext_test_support.h"
#include "localization_bayes.h"
#include "localization_eikonal.h"

/*
 * Bayesian location: a posterior region, not a point. The region must be
 * honest -- tight where the geometry resolves the source, tall in depth
 * under a flat array, wider when the velocity is uncertain -- and
 * calibrated: its standard deviations must cover the truth as often as
 * they claim.
 */

#define V_MPS 300.0
#define SIGMA_US 20.0

static const localization_point3d_t SOURCE = { 1.7, 1.4, -1.2 };


static void make_picks(
    const localization_point3d_t *sensors,
    size_t count,
    localization_point3d_t source,
    double velocity,
    double noise_us,
    uint32_t *state,
    localization_bayes_pick_t *picks
)
{
    for (size_t i = 0U; i < count; ++i) {
        const double noise = noise_us > 0.0 ? noise_us * ext_test_normal(state) : 0.0;
        picks[i].sensor_position = sensors[i];
        picks[i].arrival_time_us = (int64_t)llround((0.01 + ext_test_distance(source, sensors[i]) / velocity) * 1.0e6 + noise);
        picks[i].weight = 1.0;
    }
}


static localization_bayes_config_t config_for(double sigma_v)
{
    localization_bayes_config_t config;
    memset(&config, 0, sizeof(config));
    config.velocity_mps = V_MPS;
    config.velocity_sigma_mps = sigma_v;
    config.timing_uncertainty_us = SIGMA_US;
    config.refine_divisor = 4U;
    return config;
}


static bool inside_box(const localization_bayes_result_t *r, localization_point3d_t p, double margin)
{
    return
        p.x_m >= r->credible_min.x_m - margin && p.x_m <= r->credible_max.x_m + margin &&
        p.y_m >= r->credible_min.y_m - margin && p.y_m <= r->credible_max.y_m + margin &&
        p.z_m >= r->credible_min.z_m - margin && p.z_m <= r->credible_max.z_m + margin;
}


static void test_resolving_array_gives_a_tight_region(void)
{
    uint32_t state = 3U;
    localization_bayes_pick_t picks[6];
    make_picks(EXT_TEST_SENSORS_3D, 6U, SOURCE, V_MPS, SIGMA_US, &state, picks);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    const localization_bayes_config_t config = config_for(0.0);
    localization_bayes_result_t result;
    assert(localization_bayes_locate(picks, 6U, &bounds, &config, &result) == ESP_OK);
    printf("  6 sensors with relief: MAP error %.3f m, std (%.3f, %.3f, %.3f) m, 90%% volume %.2e m^3\n",
           ext_test_distance(result.map_position, SOURCE), result.std_m.x_m, result.std_m.y_m, result.std_m.z_m,
           result.credible_volume_m3);
    assert(result.status == LOCALIZATION_BAYES_SOLVED);
    assert(!result.planar && result.picks_used == 6U && result.pick_mask_used == 0x3FU);
    assert(ext_test_distance(result.map_position, SOURCE) < 0.06);
    assert(ext_test_distance(result.mean_position, SOURCE) < 0.1);
    assert(result.std_m.x_m < 0.1 && result.std_m.y_m < 0.1 && result.std_m.z_m < 0.1);
    assert(inside_box(&result, SOURCE, result.cell_m));
    assert(fabs(result.credible_level - 0.9) < 1e-12);
    assert(result.credible_volume_m3 > 0.0 && result.credible_area_m2 == 0.0);
    assert(result.credible_cells >= 1U);
    assert(result.timing_uncertainty_used_us >= SIGMA_US - 1e-9);
    assert(fabs(result.velocity_mean_mps - V_MPS) < 1e-6 && result.velocity_std_mps < 1e-3);
    assert(llabs(result.emission_time_us - 10000) < 200);
    assert(result.rms_residual_us < 3.0 * SIGMA_US);
}


static void test_flat_array_is_honest_about_depth(void)
{
    const localization_point3d_t source = { 1.1, 1.6, -1.3 };
    uint32_t state = 8U;
    localization_bayes_pick_t picks[4];
    make_picks(EXT_TEST_SENSORS_FLAT, 4U, source, V_MPS, SIGMA_US, &state, picks);
    const localization_extended_bounds_t bounds = { { -1.0, -1.0, -3.0 }, { 4.0, 3.5, 0.0 }, 0.1 };

    localization_bayes_config_t config = config_for(0.0);
    localization_bayes_result_t known;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &known) == ESP_OK);
    config = config_for(30.0);
    localization_bayes_result_t uncertain;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &uncertain) == ESP_OK);
    printf("  4 flat pods: std z %.2f m vs x %.2f m; 90%% volume %.3f m^3 (v known) -> %.3f m^3 (v +-30 m/s)\n",
           known.std_m.z_m, known.std_m.x_m, known.credible_volume_m3, uncertain.credible_volume_m3);
    assert(known.status == LOCALIZATION_BAYES_SOLVED);
    assert(known.std_m.z_m > 2.0 * known.std_m.x_m);
    assert(inside_box(&known, source, known.cell_m));
    assert(inside_box(&uncertain, source, uncertain.cell_m));
    assert(uncertain.credible_volume_m3 > 1.5 * known.credible_volume_m3);
    assert(uncertain.velocity_std_mps > 5.0);
}


static void test_data_update_the_velocity(void)
{
    /* The medium is 330 m/s; the prior says 300 +- 40. Six sensors with
     * relief carry enough information to pull the posterior towards 330. */
    uint32_t state = 12U;
    localization_bayes_pick_t picks[8];
    make_picks(EXT_TEST_SENSORS_3D, 8U, SOURCE, 330.0, SIGMA_US, &state, picks);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_bayes_config_t config = config_for(40.0);
    config.velocity_nodes = 9U;
    localization_bayes_result_t result;
    assert(localization_bayes_locate(picks, 8U, &bounds, &config, &result) == ESP_OK);
    printf("  velocity posterior: %.1f +- %.1f m/s (prior 300 +- 40, truth 330)\n",
           result.velocity_mean_mps, result.velocity_std_mps);
    assert(result.velocity_mean_mps > 315.0);
    assert(result.velocity_std_mps < 40.0);
    assert(ext_test_distance(result.map_position, SOURCE) < 0.15);
}


static void test_standard_deviations_are_calibrated(void)
{
    /* Monte Carlo on the planar layer: the truth should lie within 2 sd
     * of the posterior mean on both axes about 0.95^2 of the time. */
    const localization_point3d_t source = { 1.4, 1.0, 0.0 };
    const localization_extended_bounds_t plane = { { -0.5, -0.5, 0.0 }, { 3.5, 3.0, 0.0 }, 0.05 };
    localization_bayes_config_t config = config_for(0.0);
    config.timing_uncertainty_us = 200.0;
    config.inflate_on_misfit = true; /* must stay rare when the sigma is right */
    uint32_t state = 99U;
    size_t covered = 0U;
    size_t boxed = 0U;
    size_t inflated = 0U;
    const size_t trials = 60U;
    for (size_t trial = 0U; trial < trials; ++trial) {
        localization_bayes_pick_t picks[4];
        make_picks(EXT_TEST_SENSORS_FLAT, 4U, source, V_MPS, 200.0, &state, picks);
        localization_bayes_result_t result;
        assert(localization_bayes_locate(picks, 4U, &plane, &config, &result) == ESP_OK);
        assert(result.planar && result.credible_area_m2 > 0.0 && result.credible_volume_m3 == 0.0);
        if (
            fabs(result.mean_position.x_m - source.x_m) <= 2.0 * result.std_m.x_m &&
            fabs(result.mean_position.y_m - source.y_m) <= 2.0 * result.std_m.y_m
        ) {
            ++covered;
        }
        boxed += inside_box(&result, source, 0.0) ? 1U : 0U;
        inflated += result.sigma_inflation > 1.0 ? 1U : 0U;
        assert(result.chi_square_dof == 1);
    }
    const double coverage = (double)covered / (double)trials;
    const double box_coverage = (double)boxed / (double)trials;
    printf("  calibration over %zu events: 2-sd coverage %.2f (nominal 0.90), 90%% region box %.2f, inflated %zu\n",
           trials, coverage, box_coverage, inflated);
    assert(inflated <= trials / 10U);
    assert(coverage >= 0.78 && coverage <= 1.0);
    assert(box_coverage >= 0.85);
}


static void test_student_t_resists_a_wrong_pick(void)
{
    uint32_t state = 5U;
    localization_bayes_pick_t picks[8];
    make_picks(EXT_TEST_SENSORS_3D, 8U, SOURCE, V_MPS, SIGMA_US, &state, picks);
    picks[3].arrival_time_us += 4000;
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_bayes_config_t config = config_for(0.0);
    localization_bayes_result_t gaussian;
    assert(localization_bayes_locate(picks, 8U, &bounds, &config, &gaussian) == ESP_OK);
    config.likelihood = LOCALIZATION_BAYES_STUDENT_T;
    localization_bayes_result_t student;
    assert(localization_bayes_locate(picks, 8U, &bounds, &config, &student) == ESP_OK);
    printf("  4 ms wrong pick: Gaussian MAP %.3f m, Student-t MAP %.3f m\n",
           ext_test_distance(gaussian.map_position, SOURCE), ext_test_distance(student.map_position, SOURCE));
    assert(ext_test_distance(student.map_position, SOURCE) < 0.1);
    assert(ext_test_distance(gaussian.map_position, SOURCE) > 3.0 * ext_test_distance(student.map_position, SOURCE));
}


static bool deeper_than_one_metre(const localization_point3d_t *candidate, void *context)
{
    (void)context;
    return candidate->z_m <= -1.0;
}

static bool nowhere(const localization_point3d_t *candidate, void *context)
{
    (void)candidate;
    (void)context;
    return false;
}


static void test_prior_support_from_a_filter(void)
{
    const localization_point3d_t source = { 1.1, 1.6, -1.3 };
    uint32_t state = 8U;
    localization_bayes_pick_t picks[4];
    make_picks(EXT_TEST_SENSORS_FLAT, 4U, source, V_MPS, SIGMA_US, &state, picks);
    const localization_extended_bounds_t bounds = { { -1.0, -1.0, -3.0 }, { 4.0, 3.5, 0.0 }, 0.1 };
    localization_bayes_config_t config = config_for(30.0);
    localization_bayes_result_t open;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &open) == ESP_OK);
    config.candidate_filter.allowed = deeper_than_one_metre;
    localization_bayes_result_t restricted;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &restricted) == ESP_OK);
    assert(restricted.credible_max.z_m <= -1.0 + 1e-9);
    assert(restricted.credible_volume_m3 < open.credible_volume_m3);
    config.candidate_filter.allowed = nowhere;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &restricted) == ESP_OK);
    assert(restricted.status == LOCALIZATION_BAYES_NO_ALLOWED_CANDIDATE);
}


/* Eikonal travel times as the Bayesian forward model: a void wall. */
static double wall_velocity(const localization_point3d_t *p, void *context)
{
    (void)context;
    return (p->x_m >= 1.9 && p->x_m <= 2.3 && p->y_m <= 2.2) ? 0.0 : V_MPS;
}


static void test_eikonal_travel_time_model(void)
{
    static const localization_point3d_t sensors[5] = {
        { 2.8, 0.3, 0.0 }, { 3.8, 1.5, 0.0 }, { 2.9, 2.8, 0.0 }, { 0.3, 0.4, 0.0 }, { 0.4, 2.0, 0.0 },
    };
    const localization_point3d_t min = { 0.0, 0.0, 0.0 };
    const localization_point3d_t max = { 4.0, 3.0, 0.0 };
    const localization_point3d_t source = { 3.1, 1.2, 0.0 };

    localization_eikonal_model_t truth;
    assert(localization_eikonal_model_init(&truth, &min, &max, 0.025, 0U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&truth, wall_velocity, NULL);
    float *times = malloc(localization_eikonal_node_count(&truth) * sizeof(float));
    assert(localization_eikonal_travel_times(&truth, &source, times) == ESP_OK);
    localization_bayes_pick_t picks[5];
    for (size_t i = 0U; i < 5U; ++i) {
        picks[i].sensor_position = sensors[i];
        picks[i].arrival_time_us = (int64_t)llround((0.01 + localization_eikonal_time_at(&truth, times, &sensors[i])) * 1.0e6);
        picks[i].weight = 1.0;
    }
    free(times);
    localization_eikonal_model_free(&truth);

    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &min, &max, 0.1, 0U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&model, wall_velocity, NULL);
    localization_eikonal_fields_t fields;
    assert(localization_eikonal_fields_compute(&model, sensors, 5U, &fields) == ESP_OK);
    localization_eikonal_travel_time_context_t context = { &model, &fields };

    const localization_extended_bounds_t plane = { { 0.0, 0.0, 0.0 }, { 4.0, 3.0, 0.0 }, 0.05 };
    /* Picks good to 50 us, but a 0.1 m eikonal lattice against a 0.025 m
     * truth is only good to ~1% of the travel time. */
    localization_bayes_config_t config = config_for(0.0);
    config.timing_uncertainty_us = 50.0;
    localization_bayes_result_t straight;
    assert(localization_bayes_locate(picks, 5U, &plane, &config, &straight) == ESP_OK);
    config.travel_time_model.travel_time = localization_eikonal_travel_time;
    config.travel_time_model.context = &context;

    /* Pick sigma alone: honest about the picks, overconfident about the
     * model -- the truth falls outside the region. */
    localization_bayes_result_t picks_only;
    assert(localization_bayes_locate(picks, 5U, &plane, &config, &picks_only) == ESP_OK);

    /* With the model's own error: the lattice's ~1% (the measured FMM
     * error) plus a floor for a void edge moved by up to a cell at each of
     * two corners, cell / (v sqrt 6). */
    config.model_error_relative = 0.01;
    config.model_error_floor_us = 0.1 / (V_MPS * sqrt(6.0)) * 1.0e6;
    localization_bayes_result_t with_model;
    assert(localization_bayes_locate(picks, 5U, &plane, &config, &with_model) == ESP_OK);

    /* No model error given, but the misfit check widens the region. */
    config.model_error_relative = 0.0;
    config.model_error_floor_us = 0.0;
    config.inflate_on_misfit = true;
    localization_bayes_result_t inflated;
    assert(localization_bayes_locate(picks, 5U, &plane, &config, &inflated) == ESP_OK);

    printf("  wall, eikonal times: straight MAP %.3f m; region %.4f m^2 (picks only, truth %s) -> "
           "%.4f m^2 (+ model error, truth %s); misfit chi2 %.1f/%d -> sigma x%.1f, %.4f m^2 (truth %s)\n",
           ext_test_distance(straight.map_position, source), picks_only.credible_area_m2,
           inside_box(&picks_only, source, picks_only.cell_m) ? "inside" : "OUTSIDE",
           with_model.credible_area_m2, inside_box(&with_model, source, with_model.cell_m) ? "inside" : "OUTSIDE",
           inflated.chi_square, inflated.chi_square_dof, inflated.sigma_inflation, inflated.credible_area_m2,
           inside_box(&inflated, source, inflated.cell_m) ? "inside" : "OUTSIDE");
    assert(with_model.status == LOCALIZATION_BAYES_SOLVED);
    assert(ext_test_distance(with_model.map_position, source) < 0.1);
    assert(ext_test_distance(straight.map_position, source) > 3.0 * ext_test_distance(with_model.map_position, source));
    assert(!inside_box(&picks_only, source, picks_only.cell_m));
    assert(inside_box(&with_model, source, with_model.cell_m));
    assert(with_model.credible_area_m2 > picks_only.credible_area_m2);
    assert(picks_only.sigma_inflation == 1.0);             /* the check was off */
    assert(inflated.chi_square_dof == 2);                  /* 5 picks - x, y, t0 */
    assert(inflated.chi_square > 2.0 + 3.0 * sqrt(4.0));
    assert(inflated.sigma_inflation > 1.0);
    assert(fabs(inflated.sigma_inflation - sqrt(inflated.chi_square / 2.0)) < 1e-9);
    assert(inside_box(&inflated, source, inflated.cell_m));
    localization_eikonal_fields_free(&fields);
    localization_eikonal_model_free(&model);
}


static void test_model_error_widens_the_region_consistently(void)
{
    /* A relative model error makes each sigma depend on the candidate's
     * travel times; the likelihood keeps its normalization terms, so the
     * posterior stays a proper density: wider, and still consistent with
     * the truth (within its own sd), not pushed towards large sigmas. */
    uint32_t state = 3U;
    localization_bayes_pick_t picks[6];
    make_picks(EXT_TEST_SENSORS_3D, 6U, SOURCE, V_MPS, SIGMA_US, &state, picks);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_bayes_config_t config = config_for(0.0);
    config.model_error_relative = 0.05;
    config.model_error_floor_us = 30.0;
    localization_bayes_result_t result;
    assert(localization_bayes_locate(picks, 6U, &bounds, &config, &result) == ESP_OK);
    printf("  model error 5%% + 30 us: MAP off by %.3f m, sd (%.2f, %.2f, %.2f) m\n",
           ext_test_distance(result.map_position, SOURCE), result.std_m.x_m, result.std_m.y_m, result.std_m.z_m);
    assert(fabs(result.mean_position.x_m - SOURCE.x_m) <= 2.0 * result.std_m.x_m);
    assert(fabs(result.mean_position.y_m - SOURCE.y_m) <= 2.0 * result.std_m.y_m);
    assert(fabs(result.mean_position.z_m - SOURCE.z_m) <= 2.0 * result.std_m.z_m);
    assert(inside_box(&result, SOURCE, result.cell_m));
    assert(result.chi_square_dof == 2 && result.sigma_inflation == 1.0);
    /* ... and the region is wider than with an exact model. */
    localization_bayes_config_t exact = config_for(0.0);
    localization_bayes_result_t tight;
    assert(localization_bayes_locate(picks, 6U, &bounds, &exact, &tight) == ESP_OK);
    assert(result.credible_volume_m3 > tight.credible_volume_m3);

    config.model_error_relative = -0.1;
    assert(localization_bayes_locate(picks, 6U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
}


static void test_invalid_input(void)
{
    uint32_t state = 1U;
    localization_bayes_pick_t picks[4];
    make_picks(EXT_TEST_SENSORS_FLAT, 4U, SOURCE, V_MPS, 0.0, &state, picks);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_bayes_config_t config = config_for(0.0);
    localization_bayes_result_t result;
    assert(localization_bayes_locate(picks, 3U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_BAYES_INSUFFICIENT_PICKS);
    assert(localization_bayes_locate(NULL, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_bayes_locate(picks, 9U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config.velocity_nodes = 4U;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = config_for(0.0);
    config.credible_level = 1.0;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = config_for(-1.0);
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
    config = config_for(0.0);
    config.refine_divisor = 1U;
    assert(localization_bayes_locate(picks, 4U, &bounds, &config, &result) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_resolving_array_gives_a_tight_region();
    test_flat_array_is_honest_about_depth();
    test_data_update_the_velocity();
    test_standard_deviations_are_calibrated();
    test_student_t_resists_a_wrong_pick();
    test_prior_support_from_a_filter();
    test_eikonal_travel_time_model();
    test_model_error_widens_the_region_consistently();
    test_invalid_input();
    printf("localization Bayesian tests passed\n");
    return 0;
}
