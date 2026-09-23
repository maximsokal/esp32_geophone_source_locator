/*
 * MAX-136: synthetic ground-truth validation of the statistical
 * traveltime-inversion grid-search backend. Builds exact differential
 * arrival times for a KNOWN source position and checks the coarse-to-fine
 * search recovers it, that a bad pick can be down-weighted away rather than
 * corrupting the answer, and that the retained coarse-grid surface is a
 * real per-candidate surface (not a stand-in for a point estimate).
 */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define TEST_PRINTF(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

#include "localization_traveltime_inversion.h"


static const double s_speed_mps = 340.0;
static const double s_timing_uncertainty_us = 50.0;
static const int64_t s_emission_time_us = 500000;


static int64_t true_arrival_time_us(
    localization_point3d_t sensor,
    localization_point3d_t source,
    double speed_mps,
    int64_t emission_time_us
)
{
    const double dx = source.x_m - sensor.x_m;
    const double dy = source.y_m - sensor.y_m;
    const double dz = source.z_m - sensor.z_m;
    const double distance = sqrt(dx * dx + dy * dy + dz * dz);
    return emission_time_us + (int64_t)llround(distance / speed_mps * 1.0e6);
}


static double position_error_m(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


static const localization_point3d_t s_sensors3[3] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.0 },
    { 0.0, 4.0, 0.0 }
};
static const localization_point3d_t s_source2d = { 1.2, 1.6, 0.0 };

static const localization_point3d_t s_sensors4[4] = {
    { 0.0, 0.0, 0.0 },
    { 4.0, 0.0, 0.0 },
    { 0.0, 4.0, 0.0 },
    { 2.0, 2.0, 4.0 }
};
static const localization_point3d_t s_source3d = { 1.0, 1.2, 1.5 };


static void test_planar_search_recovers_known_source_in_2d(void)
{
    localization_traveltime_pick_t picks[3];
    for (size_t i = 0u; i < 3u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors3[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors3[i], s_source2d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }

    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .retain_surface = true
    };

    localization_traveltime_result_t result;
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &config, &result) == ESP_OK
    );

    assert(result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D);
    assert(result.picks_used_count == 3u);
    assert(result.best_position.z_m == 0.0);
    assert(result.surface != NULL);
    assert(result.surface_count == 9u * 9u);

    const double error_m = position_error_m(result.best_position, s_source2d);
    TEST_PRINTF(
        "[MAX-136] planar search: best=(%.3f,%.3f) true=(%.3f,%.3f) error=%.4f m "
        "residual=%.3f us surface_count=%zu\n",
        result.best_position.x_m, result.best_position.y_m,
        s_source2d.x_m, s_source2d.y_m, error_m,
        result.best_residual_rms_us, result.surface_count
    );

    assert(error_m < 0.1);
    assert(result.best_residual_rms_us < 1.0);

    free(result.surface);
}


static void test_3d_search_recovers_known_source(void)
{
    localization_traveltime_pick_t picks[4];
    for (size_t i = 0u; i < 4u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors4[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors4[i], s_source3d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }

    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 4.0 },
        .step_m = 1.0
    };
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .retain_surface = true
    };

    localization_traveltime_result_t result;
    assert(
        localization_traveltime_search(picks, 4u, &bounds, &config, &result) == ESP_OK
    );

    assert(result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_CAPABLE_3D);
    assert(result.picks_used_count == 4u);
    assert(result.surface != NULL);
    assert(result.surface_count == 5u * 5u * 5u);

    const double error_m = position_error_m(result.best_position, s_source3d);
    TEST_PRINTF(
        "[MAX-136] 3D search: best=(%.3f,%.3f,%.3f) error=%.4f m residual=%.3f us\n",
        result.best_position.x_m, result.best_position.y_m, result.best_position.z_m,
        error_m, result.best_residual_rms_us
    );

    assert(error_m < 0.3);
    assert(result.best_residual_rms_us < 1.0);

    free(result.surface);
}


static void test_insufficient_picks_after_weighting_is_reported(void)
{
    /* 4 physically wired picks, but only 2 usable (weight <= 0 excludes the
     * other 2) -- below LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D. */
    localization_traveltime_pick_t picks[4];
    for (size_t i = 0u; i < 4u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors4[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors4[i], s_source3d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = (i < 2u) ? 1.0 : 0.0
        };
    }

    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 4.0 },
        .step_m = 1.0
    };
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .retain_surface = true
    };

    localization_traveltime_result_t result;
    assert(
        localization_traveltime_search(picks, 4u, &bounds, &config, &result) == ESP_OK
    );

    assert(result.status == LOCALIZATION_TRAVELTIME_INSUFFICIENT_PICKS);
    assert(result.dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT);
    assert(result.picks_used_count == 2u);
    assert(result.surface == NULL);
    assert(result.surface_count == 0u);
}


static void test_down_weighted_bad_pick_is_excluded_from_hijacking_the_estimate(void)
{
    /* Sensor 3's pick is badly corrupted (a stuck/mistimed channel). With
     * full weight it must pull the estimate off; down-weighted to ~0 it
     * must not. */
    const int64_t corruption_us = 5000; /* far larger than s_timing_uncertainty_us */

    localization_traveltime_pick_t bad_picks[4];
    localization_traveltime_pick_t down_weighted_picks[4];
    for (size_t i = 0u; i < 4u; ++i) {
        int64_t arrival = true_arrival_time_us(
            s_sensors4[i], s_source3d, s_speed_mps, s_emission_time_us
        );
        if (i == 3u) {
            arrival += corruption_us;
        }

        const localization_observation_t observation = {
            .sensor_position = s_sensors4[i],
            .arrival_time_us = arrival
        };
        bad_picks[i] = (localization_traveltime_pick_t) {
            .observation = observation, .weight = 1.0
        };
        down_weighted_picks[i] = (localization_traveltime_pick_t) {
            .observation = observation, .weight = (i == 3u) ? 0.001 : 1.0
        };
    }

    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 4.0 },
        .step_m = 1.0
    };
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .retain_surface = true
    };

    localization_traveltime_result_t corrupted_result;
    assert(
        localization_traveltime_search(bad_picks, 4u, &bounds, &config, &corrupted_result) ==
        ESP_OK
    );
    assert(corrupted_result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    const double corrupted_error_m =
        position_error_m(corrupted_result.best_position, s_source3d);

    localization_traveltime_result_t corrected_result;
    assert(
        localization_traveltime_search(
            down_weighted_picks, 4u, &bounds, &config, &corrected_result
        ) == ESP_OK
    );
    assert(corrected_result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    const double corrected_error_m =
        position_error_m(corrected_result.best_position, s_source3d);

    TEST_PRINTF(
        "[MAX-136] bad-pick handling: full-weight error=%.4f m "
        "down-weighted error=%.4f m\n",
        corrupted_error_m, corrected_error_m
    );

    assert(corrected_error_m < corrupted_error_m);
    assert(corrected_error_m < 0.3);

    free(corrupted_result.surface);
    free(corrected_result.surface);
}


static void test_surface_has_real_per_candidate_variation(void)
{
    localization_traveltime_pick_t picks[3];
    for (size_t i = 0u; i < 3u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors3[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors3[i], s_source2d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }

    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    /* A much looser timing_uncertainty_us than the recovery tests use: at
     * 50 us (a few cm of resolvable distance at this speed), a 0.5 m coarse
     * grid decays to indistinguishable near-zero likelihood everywhere
     * except right at the peak, which would make THIS test -- whether the
     * surface has real graded variation, not just a single spike -- trivial
     * to pass by accident. 2000 us keeps neighboring coarse candidates'
     * residuals within a few sigma of each other, showing genuine grading. */
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = 2000.0,
        .refine_divisor = 10u,
        .retain_surface = true
    };

    localization_traveltime_result_t result;
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &config, &result) == ESP_OK
    );
    assert(result.surface_count == 9u * 9u);

    double min_likelihood = result.surface[0].relative_likelihood;
    double max_likelihood = result.surface[0].relative_likelihood;
    for (size_t i = 1u; i < result.surface_count; ++i) {
        const double likelihood = result.surface[i].relative_likelihood;
        if (likelihood < min_likelihood) {
            min_likelihood = likelihood;
        }
        if (likelihood > max_likelihood) {
            max_likelihood = likelihood;
        }
    }

    TEST_PRINTF(
        "[MAX-136] surface variation: min=%.6f max=%.6f (count=%zu)\n",
        min_likelihood, max_likelihood, result.surface_count
    );

    /* A stub that always wrote the same value would pass a "count" check
     * but not this -- the surface must actually vary across the grid. */
    assert(max_likelihood - min_likelihood > 0.1);
    /* relative_likelihood is normalized against the GLOBAL best (which may
     * be an off-grid fine-refinement point), so even the best coarse
     * candidate is typically < 1.0, never > 1.0. */
    assert(max_likelihood <= 1.0 + 1e-9);

    free(result.surface);
}


static void test_extreme_int64_arrival_difference_is_safe(void)
{
    const localization_traveltime_pick_t picks[3] = {
        {
            .observation = {
                .sensor_position = s_sensors3[0],
                .arrival_time_us = INT64_MIN
            },
            .weight = 1.0
        },
        {
            .observation = {
                .sensor_position = s_sensors3[1],
                .arrival_time_us = INT64_MAX
            },
            .weight = 1.0
        },
        {
            .observation = {
                .sensor_position = s_sensors3[2],
                .arrival_time_us = INT64_MAX
            },
            .weight = 1.0
        }
    };
    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 0.0, 0.0, 0.0 },
        .step_m = 1.0
    };
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 2u,
        .retain_surface = true
    };
    localization_traveltime_result_t result;

    assert(localization_traveltime_search(
        picks, 3u, &bounds, &config, &result
    ) == ESP_OK);
    assert(result.surface != NULL);
    assert(result.surface_count == 1u);
    assert(isfinite(result.best_residual_rms_us));
    free(result.surface);
}


static void test_rejects_invalid_config_and_null_arguments(void)
{
    localization_traveltime_pick_t picks[3];
    for (size_t i = 0u; i < 3u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors3[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors3[i], s_source2d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }

    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .retain_surface = true
    };

    localization_traveltime_result_t result;

    assert(
        localization_traveltime_search(NULL, 3u, &bounds, &config, &result) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_traveltime_search(picks, 3u, NULL, &config, &result) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_traveltime_search(picks, 3u, &bounds, NULL, &result) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &config, NULL) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_traveltime_search(picks, 0u, &bounds, &config, &result) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_traveltime_search(
            picks, LOCALIZATION_TRAVELTIME_MAX_PICKS + 1u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_traveltime_config_t bad_speed = config;
    bad_speed.propagation_speed_mps = 0.0;
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &bad_speed, &result) ==
        ESP_ERR_INVALID_ARG
    );
    bad_speed.propagation_speed_mps = NAN;
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &bad_speed, &result) ==
        ESP_ERR_INVALID_ARG
    );

    localization_traveltime_config_t bad_uncertainty = config;
    bad_uncertainty.timing_uncertainty_us = 0.0;
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &bad_uncertainty, &result) ==
        ESP_ERR_INVALID_ARG
    );
    bad_uncertainty.timing_uncertainty_us = INFINITY;
    assert(
        localization_traveltime_search(
            picks, 3u, &bounds, &bad_uncertainty, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_traveltime_config_t bad_divisor = config;
    bad_divisor.refine_divisor = 1u;
    assert(
        localization_traveltime_search(picks, 3u, &bounds, &bad_divisor, &result) ==
        ESP_ERR_INVALID_ARG
    );

    localization_traveltime_search_bounds_t bad_step = bounds;
    bad_step.step_m = 0.0;
    assert(
        localization_traveltime_search(picks, 3u, &bad_step, &config, &result) ==
        ESP_ERR_INVALID_ARG
    );

    localization_traveltime_search_bounds_t reversed_bounds = bounds;
    reversed_bounds.max_corner.x_m = reversed_bounds.min_corner.x_m - 1.0;
    assert(
        localization_traveltime_search(
            picks, 3u, &reversed_bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_traveltime_search_bounds_t huge_bounds = bounds;
    huge_bounds.max_corner.x_m = DBL_MAX;
    huge_bounds.step_m = 1.0;
    assert(
        localization_traveltime_search(
            picks, 3u, &huge_bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_traveltime_pick_t bad_weight[3] = {
        picks[0], picks[1], picks[2]
    };
    bad_weight[0].weight = NAN;
    assert(
        localization_traveltime_search(
            bad_weight, 3u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );

    localization_traveltime_pick_t bad_position[3] = {
        picks[0], picks[1], picks[2]
    };
    bad_position[0].observation.sensor_position.x_m = INFINITY;
    assert(
        localization_traveltime_search(
            bad_position, 3u, &bounds, &config, &result
        ) == ESP_ERR_INVALID_ARG
    );
}


/* MAX-208: the production search box for a 5x5 m 3D array at a 0.1 m step
 * was X=122, Y=122, Z=72 = 1,071,648 candidates (~49 MiB with a retained
 * surface) -- guaranteed ESP_ERR_NO_MEM on 8 MiB PSRAM. The streaming search
 * must solve it with no surface and within the candidate budget. */
static const localization_point3d_t s_sensors_5x5[4] = {
    { 0.0, 0.0, 0.0 },
    { 5.0, 0.0, 0.2 },
    { 0.0, 5.0, -0.1 },
    { 5.0, 5.0, 0.3 }
};

static void fill_picks_5x5(localization_traveltime_pick_t picks[4], localization_point3d_t source)
{
    for (size_t i = 0u; i < 4u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors_5x5[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors_5x5[i], source, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }
}

static const localization_traveltime_search_bounds_t s_bounds_5x5 = {
    .min_corner = { -3.5355, -3.5355, -3.6355 },
    .max_corner = { 8.5355, 8.5355, 3.8355 },
    .step_m = 0.1
};

static void test_realistic_3d_grid_streams_within_budget(void)
{
    const localization_point3d_t source = { 2.0, 3.0, -1.0 };
    localization_traveltime_pick_t picks[4];
    fill_picks_5x5(picks, source);

    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 4u,
    };
    localization_traveltime_result_t result;
    assert(localization_traveltime_search(picks, 4u, &s_bounds_5x5, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    assert(result.surface == NULL && result.surface_count == 0u);
    /* 1.07 M candidates exceed the default 200k budget: the step grows. */
    assert(result.coarse_step_m_used > 0.1);

    const double dx = result.best_position.x_m - source.x_m;
    const double dy = result.best_position.y_m - source.y_m;
    const double horizontal_error_m = sqrt(dx * dx + dy * dy);
    TEST_PRINTF(
        "[MAX-208] streaming 5x5 m 3D: step_used=%.2f m best=(%.2f,%.2f,%.2f) horizontal_error=%.4f m\n",
        result.coarse_step_m_used, result.best_position.x_m, result.best_position.y_m,
        result.best_position.z_m, horizontal_error_m
    );
    /* The fine pass restores sub-coarse-step horizontal resolution. Depth
     * SIGN is not asserted: a near-coplanar surface array cannot tell a
     * source below it from its mirror above it (the MAX-211 below-surface
     * search policy handles that, not this backend). */
    assert(horizontal_error_m < 0.25);
}

static void test_retained_surface_is_capped(void)
{
    const localization_point3d_t source = { 2.0, 3.0, -1.0 };
    localization_traveltime_pick_t picks[4];
    fill_picks_5x5(picks, source);

    const localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 4u,
        .retain_surface = true
    };
    localization_traveltime_result_t result;
    assert(localization_traveltime_search(picks, 4u, &s_bounds_5x5, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    assert(result.surface != NULL);
    assert(result.surface_count <= LOCALIZATION_TRAVELTIME_MAX_SURFACE_CANDIDATES);
    free(result.surface);
}

static void test_streaming_matches_retained_on_small_grid(void)
{
    localization_traveltime_pick_t picks[4];
    for (size_t i = 0u; i < 4u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors4[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors4[i], s_source3d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }
    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 4.0 },
        .step_m = 1.0
    };
    localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .retain_surface = true
    };
    localization_traveltime_result_t retained;
    assert(localization_traveltime_search(picks, 4u, &bounds, &config, &retained) == ESP_OK);
    config.retain_surface = false;
    localization_traveltime_result_t streamed;
    assert(localization_traveltime_search(picks, 4u, &bounds, &config, &streamed) == ESP_OK);

    assert(streamed.surface == NULL);
    assert(streamed.coarse_step_m_used == 1.0 && retained.coarse_step_m_used == 1.0);
    assert(streamed.best_position.x_m == retained.best_position.x_m);
    assert(streamed.best_position.y_m == retained.best_position.y_m);
    assert(streamed.best_position.z_m == retained.best_position.z_m);
    free(retained.surface);
}


/* MAX-147: a candidate filter excludes grid points (e.g. scan-verified FREE
 * air); the search returns the best ADMITTED point, and reports when the
 * filter admits nothing at all. */
static bool reject_x_below_2(const localization_point3d_t *candidate, void *context)
{
    (void)context;
    return candidate->x_m >= 2.0;
}

static bool reject_everything(const localization_point3d_t *candidate, void *context)
{
    (void)candidate;
    (void)context;
    return false;
}

static void test_candidate_filter_constrains_the_answer(void)
{
    localization_traveltime_pick_t picks[3];
    for (size_t i = 0u; i < 3u; ++i) {
        picks[i] = (localization_traveltime_pick_t) {
            .observation = {
                .sensor_position = s_sensors3[i],
                .arrival_time_us = true_arrival_time_us(
                    s_sensors3[i], s_source2d, s_speed_mps, s_emission_time_us
                )
            },
            .weight = 1.0
        };
    }
    const localization_traveltime_search_bounds_t bounds = {
        .min_corner = { 0.0, 0.0, 0.0 },
        .max_corner = { 4.0, 4.0, 0.0 },
        .step_m = 0.5
    };
    localization_traveltime_config_t config = {
        .propagation_speed_mps = s_speed_mps,
        .timing_uncertainty_us = s_timing_uncertainty_us,
        .refine_divisor = 10u,
        .candidate_filter = { .allowed = reject_x_below_2, .context = NULL },
    };
    localization_traveltime_result_t result;
    assert(localization_traveltime_search(picks, 3u, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TRAVELTIME_SOLVED);
    assert(result.best_position.x_m >= 2.0);

    config.candidate_filter.allowed = reject_everything;
    assert(localization_traveltime_search(picks, 3u, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_TRAVELTIME_NO_ALLOWED_CANDIDATE);
    assert(result.surface == NULL);
}


int main(void)
{
    test_candidate_filter_constrains_the_answer();
    test_realistic_3d_grid_streams_within_budget();
    test_retained_surface_is_capped();
    test_streaming_matches_retained_on_small_grid();
    test_planar_search_recovers_known_source_in_2d();
    test_3d_search_recovers_known_source();
    test_insufficient_picks_after_weighting_is_reported();
    test_down_weighted_bad_pick_is_excluded_from_hijacking_the_estimate();
    test_surface_has_real_per_candidate_variation();
    test_extreme_int64_arrival_difference_is_safe();
    test_rejects_invalid_config_and_null_arguments();
    puts("localization traveltime inversion tests passed");
    return 0;
}
