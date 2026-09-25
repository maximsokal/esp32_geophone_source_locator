#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext_test_support.h"
#include "localization_eikonal.h"

/*
 * Eikonal travel times (fast marching), location in a velocity model with
 * voids, and travel-time tomography from calibration hits. Ground truth for
 * the heterogeneous cases comes from an independent finer model (half the
 * cell size), so the tests do not grade the solver against itself.
 */

#define V_MPS 300.0
#define SIGMA_US 20.0


static double max_relative_error_homogeneous(const localization_point3d_t *min, const localization_point3d_t *max, localization_point3d_t source)
{
    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, min, max, 0.1, 0U, V_MPS) == ESP_OK);
    const size_t count = localization_eikonal_node_count(&model);
    float *times = malloc(count * sizeof(float));
    assert(times != NULL);
    assert(localization_eikonal_travel_times(&model, &source, times) == ESP_OK);
    double worst = 0.0;
    for (size_t i = 0U; i < count; ++i) {
        const localization_point3d_t p = localization_eikonal_node_position(&model, i);
        const double exact = ext_test_distance(p, source) / V_MPS;
        if (exact * V_MPS > 0.3) {
            worst = fmax(worst, fabs(times[i] - exact) / exact);
        }
    }
    /* Interpolated times between nodes. */
    uint32_t state = 5U;
    for (int k = 0; k < 200; ++k) {
        const localization_point3d_t p = {
            min->x_m + 0.5 * (ext_test_uniform(&state, 1.0) + 1.0) * (max->x_m - min->x_m),
            min->y_m + 0.5 * (ext_test_uniform(&state, 1.0) + 1.0) * (max->y_m - min->y_m),
            min->z_m + 0.5 * (ext_test_uniform(&state, 1.0) + 1.0) * (max->z_m - min->z_m),
        };
        const double exact = ext_test_distance(p, source) / V_MPS;
        if (exact * V_MPS > 0.3) {
            worst = fmax(worst, fabs(localization_eikonal_time_at(&model, times, &p) - exact) / exact);
        }
    }
    free(times);
    localization_eikonal_model_free(&model);
    return worst;
}


static void test_homogeneous_accuracy(void)
{
    const localization_point3d_t min3 = { 0.0, 0.0, -1.5 };
    const localization_point3d_t max3 = { 3.0, 3.0, 0.0 };
    const localization_point3d_t source3 = { 0.55, 0.62, -0.73 };
    const double e3 = max_relative_error_homogeneous(&min3, &max3, source3);

    const localization_point3d_t min2 = { 0.0, 0.0, 0.0 };
    const localization_point3d_t max2 = { 4.0, 3.0, 0.0 };
    const localization_point3d_t source2 = { 1.23, 0.87, 0.0 };
    const double e2 = max_relative_error_homogeneous(&min2, &max2, source2);
    printf("  homogeneous: max relative error 3D %.2f%%, 2D %.2f%%\n", 100.0 * e3, 100.0 * e2);
    assert(e3 < 0.02);
    assert(e2 < 0.02);
}


static void test_wave_goes_around_a_void(void)
{
    /* A 2D slab with a wall of void from y = 0 to y = 2 at x = 2; the
     * wave from (1, 1) reaches (3, 1) around the wall's end at (2, 2). */
    const localization_point3d_t min = { 0.0, 0.0, 0.0 };
    const localization_point3d_t max = { 4.0, 3.0, 0.0 };
    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &min, &max, 0.05, 0U, V_MPS) == ESP_OK);
    assert(model.nz == 1U && model.nx == 81U && model.ny == 61U);
    const size_t count = localization_eikonal_node_count(&model);
    for (size_t i = 0U; i < count; ++i) {
        const localization_point3d_t p = localization_eikonal_node_position(&model, i);
        if (fabs(p.x_m - 2.0) < 0.06 && p.y_m <= 2.0 + 1e-9) {
            localization_eikonal_set_velocity(&model, i, 0.0);
        }
    }
    assert(localization_eikonal_passable_count(&model) < count);
    float *times = malloc(count * sizeof(float));
    const localization_point3d_t source = { 1.0, 1.0, 0.0 };
    const localization_point3d_t sensor = { 3.0, 1.0, 0.0 };
    assert(localization_eikonal_travel_times(&model, &source, times) == ESP_OK);
    const localization_point3d_t corner = { 2.0, 2.05, 0.0 };
    const double around = (ext_test_distance(source, corner) + ext_test_distance(corner, sensor)) / V_MPS;
    const double straight = ext_test_distance(source, sensor) / V_MPS;
    const double t = localization_eikonal_time_at(&model, times, &sensor);
    printf("  around a wall: %.3f ms (straight %.3f, analytic detour %.3f)\n", t * 1e3, straight * 1e3, around * 1e3);
    assert(fabs(t - around) / around < 0.03);
    assert(t > 1.3 * straight);

    /* A void is never entered: its nodes stay unreachable. */
    const localization_point3d_t in_wall = { 2.0, 1.0, 0.0 };
    size_t wall_node = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const localization_point3d_t p = localization_eikonal_node_position(&model, i);
        if (fabs(p.x_m - in_wall.x_m) < 1e-9 && fabs(p.y_m - in_wall.y_m) < 1e-9) {
            wall_node = i;
        }
    }
    assert(isinf(times[wall_node]));
    assert(localization_eikonal_velocity(&model, wall_node) == 0.0);

    /* Sources outside the lattice or sealed in a void are refused. */
    const localization_point3d_t outside = { 9.0, 1.0, 0.0 };
    assert(localization_eikonal_travel_times(&model, &outside, times) == ESP_ERR_NOT_FOUND);
    free(times);
    localization_eikonal_model_free(&model);
}


/* ---- A trench scene in 3D ------------------------------------------- */

/* Void: a trench across the whole y range at x in [1.9, 2.3], down to
 * z = -1.0; sensors on both sides; source right of it. */
static double trench_velocity(const localization_point3d_t *p, void *context)
{
    (void)context;
    return (p->x_m >= 1.9 - 1e-9 && p->x_m <= 2.3 + 1e-9 && p->z_m >= -1.0 - 1e-9) ? 0.0 : V_MPS;
}

static double homogeneous_velocity(const localization_point3d_t *p, void *context)
{
    (void)p;
    (void)context;
    return V_MPS;
}

static const localization_point3d_t TRENCH_SENSORS[6] = {
    { 2.6, 0.2, 0.0 }, { 3.8, 0.4, 0.0 }, { 2.7, 2.8, 0.0 }, { 3.9, 2.6, 0.0 },
    { 0.4, 0.6, 0.0 }, { 0.5, 2.5, 0.0 },
};
static const localization_point3d_t TRENCH_MIN = { 0.0, 0.0, -2.0 };
static const localization_point3d_t TRENCH_MAX = { 4.2, 3.0, 0.0 };


/* Picks from a model at `cell_m` (the "truth" when finer than the locator's). */
static void trench_picks(
    localization_point3d_t source,
    double cell_m,
    uint32_t *state,
    localization_eikonal_pick_t *picks
)
{
    localization_eikonal_model_t truth;
    assert(localization_eikonal_model_init(&truth, &TRENCH_MIN, &TRENCH_MAX, cell_m, 200000U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&truth, trench_velocity, NULL);
    float *times = malloc(localization_eikonal_node_count(&truth) * sizeof(float));
    assert(localization_eikonal_travel_times(&truth, &source, times) == ESP_OK);
    for (size_t i = 0U; i < 6U; ++i) {
        const double t = localization_eikonal_time_at(&truth, times, &TRENCH_SENSORS[i]);
        assert(isfinite(t));
        picks[i].arrival_time_us = (int64_t)llround((0.01 + t) * 1.0e6 + SIGMA_US * ext_test_normal(state));
        picks[i].weight = 1.0;
    }
    free(times);
    localization_eikonal_model_free(&truth);
}


static localization_eikonal_locate_config_t locate_config(void)
{
    localization_eikonal_locate_config_t config;
    memset(&config, 0, sizeof(config));
    config.timing_uncertainty_us = SIGMA_US;
    config.velocity_scale_sigma = 0.0;
    config.refine_divisor = 4U;
    return config;
}


static double locate_in(
    localization_eikonal_velocity_fn velocity,
    const localization_eikonal_pick_t *picks,
    localization_point3d_t source,
    localization_eikonal_result_t *result
)
{
    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &TRENCH_MIN, &TRENCH_MAX, 0.1, 0U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&model, velocity, NULL);
    localization_eikonal_fields_t fields;
    assert(localization_eikonal_fields_compute(&model, TRENCH_SENSORS, 6U, &fields) == ESP_OK);
    const localization_eikonal_locate_config_t config = locate_config();
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, result) == ESP_OK);
    localization_eikonal_fields_free(&fields);
    localization_eikonal_model_free(&model);
    return ext_test_distance(result->position, source);
}


static void test_location_through_scan_voids(void)
{
    const localization_point3d_t source = { 3.1, 1.4, -0.7 };
    uint32_t state = 42U;
    localization_eikonal_pick_t picks[6];
    trench_picks(source, 0.05, &state, picks);

    localization_eikonal_result_t with_void;
    localization_eikonal_result_t straight;
    const double void_error = locate_in(trench_velocity, picks, source, &with_void);
    const double straight_error = locate_in(homogeneous_velocity, picks, source, &straight);
    printf("  trench between source and 2 sensors: straight rays %.3f m, eikonal with void %.3f m\n",
           straight_error, void_error);
    assert(with_void.status == LOCALIZATION_EIKONAL_SOLVED);
    assert(with_void.picks_used == 6U && with_void.pick_mask_used == 0x3FU);
    assert(void_error < 0.1);
    assert(straight_error > 3.0 * void_error);
    assert(with_void.rms_residual_us < straight.rms_residual_us);
    assert(fabs(with_void.velocity_scale - 1.0) < 1e-12); /* fixed */
    assert(llabs(with_void.emission_time_us - 10000) < 150);
}


static void test_free_velocity_scale_and_robust_pick(void)
{
    /* The medium is 10% faster than modelled: a free scale absorbs it. */
    const localization_point3d_t source = { 3.1, 1.4, -0.7 };
    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &TRENCH_MIN, &TRENCH_MAX, 0.1, 0U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&model, trench_velocity, NULL);
    localization_eikonal_fields_t fields;
    assert(localization_eikonal_fields_compute(&model, TRENCH_SENSORS, 6U, &fields) == ESP_OK);
    localization_eikonal_pick_t picks[6];
    for (size_t i = 0U; i < 6U; ++i) {
        const double t = localization_eikonal_time_at(&model, fields.fields[i], &source) / 1.1;
        picks[i].arrival_time_us = (int64_t)llround((0.01 + t) * 1.0e6);
        picks[i].weight = 1.0;
    }
    localization_eikonal_locate_config_t config = locate_config();
    config.velocity_scale_sigma = -1.0;
    localization_eikonal_result_t result;
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EIKONAL_SOLVED);
    assert(fabs(result.velocity_scale - 1.0 / 1.1) < 0.01);
    assert(ext_test_distance(result.position, source) < 0.05);

    /* One reflected pick 3 ms late, velocity scale known (two picks to
     * spare): the leave-one-out test rejects it. */
    picks[1].arrival_time_us += 3000;
    config.velocity_scale_sigma = 0.0;
    for (size_t i = 0U; i < 6U; ++i) {
        const double t = localization_eikonal_time_at(&model, fields.fields[i], &source);
        picks[i].arrival_time_us = (int64_t)llround((0.01 + t) * 1.0e6) + (i == 1U ? 3000 : 0);
    }
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, &result) == ESP_OK);
    printf("  late pick: error %.3f m, rejected mask 0x%x\n",
           ext_test_distance(result.position, source), (unsigned)result.pick_mask_rejected);
    assert(result.pick_mask_rejected == (1U << 1));
    assert(result.picks_used == 5U && result.pick_mask_used == 0x3DU);
    assert(ext_test_distance(result.position, source) < 0.05);

    /* The travel-time adaptor serves other backends. */
    localization_eikonal_travel_time_context_t context = { &model, &fields };
    double t = 0.0;
    assert(localization_eikonal_travel_time(2U, &source, &t, &context));
    assert(fabs(t - localization_eikonal_time_at(&model, fields.fields[2], &source)) < 1e-12);
    assert(!localization_eikonal_travel_time(6U, &source, &t, &context));
    const localization_point3d_t in_void = { 2.1, 1.0, -0.5 };
    assert(!localization_eikonal_travel_time(0U, &in_void, &t, &context));

    /* Scale fixed: (x, y, z, t0) needs 4 picks. */
    for (size_t i = 4U; i < 6U; ++i) {
        picks[i].weight = 0.0;
    }
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EIKONAL_SOLVED);
    picks[3].weight = 0.0;
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EIKONAL_INSUFFICIENT_PICKS);

    config.refine_divisor = 1U;
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, &result) == ESP_ERR_INVALID_ARG);
    localization_eikonal_fields_free(&fields);
    localization_eikonal_model_free(&model);
}


/* ---- Calibration ---------------------------------------------------- */

/* A 2D slab: a loosened (slow, 150 m/s) disc in 300 m/s material. */
static double slow_disc_velocity(const localization_point3d_t *p, void *context)
{
    (void)context;
    const double dx = p->x_m - 2.0;
    const double dy = p->y_m - 1.9;
    return dx * dx + dy * dy <= 0.8 * 0.8 ? 150.0 : V_MPS;
}

static const localization_point3d_t SLAB_MIN = { 0.0, 0.0, 0.0 };
static const localization_point3d_t SLAB_MAX = { 4.0, 3.8, 0.0 };
static const localization_point3d_t SLAB_SENSORS[8] = {
    { 0.1, 0.1, 0.0 }, { 2.0, 0.1, 0.0 }, { 3.9, 0.1, 0.0 }, { 3.9, 1.9, 0.0 },
    { 3.9, 3.7, 0.0 }, { 2.0, 3.7, 0.0 }, { 0.1, 3.7, 0.0 }, { 0.1, 1.9, 0.0 },
};


static void truth_arrivals(localization_point3d_t source, double origin_s, int64_t *arrival_us)
{
    localization_eikonal_model_t truth;
    assert(localization_eikonal_model_init(&truth, &SLAB_MIN, &SLAB_MAX, 0.05, 0U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&truth, slow_disc_velocity, NULL);
    float *times = malloc(localization_eikonal_node_count(&truth) * sizeof(float));
    assert(localization_eikonal_travel_times(&truth, &source, times) == ESP_OK);
    for (size_t s = 0U; s < 8U; ++s) {
        arrival_us[s] = (int64_t)llround((origin_s + localization_eikonal_time_at(&truth, times, &SLAB_SENSORS[s])) * 1.0e6);
    }
    free(times);
    localization_eikonal_model_free(&truth);
}


static double locate_slab(const localization_eikonal_model_t *model, localization_point3d_t source)
{
    int64_t arrival[8];
    truth_arrivals(source, 0.05, arrival);
    localization_eikonal_fields_t fields;
    assert(localization_eikonal_fields_compute(model, SLAB_SENSORS, 8U, &fields) == ESP_OK);
    localization_eikonal_pick_t picks[8];
    for (size_t s = 0U; s < 8U; ++s) {
        picks[s].arrival_time_us = arrival[s];
        picks[s].weight = 1.0;
    }
    const localization_eikonal_locate_config_t config = locate_config();
    localization_eikonal_result_t result;
    assert(localization_eikonal_locate(model, &fields, picks, 8U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EIKONAL_SOLVED);
    localization_eikonal_fields_free(&fields);
    return ext_test_distance(result.position, source);
}


static void test_calibration_hits_map_a_slow_zone(void)
{
    /* Twelve hammer hits at known points around and across the slab
     * (half with a trigger-known origin time). */
    localization_eikonal_shot_t shots[12];
    memset(shots, 0, sizeof(shots));
    for (size_t k = 0U; k < 12U; ++k) {
        const localization_point3d_t p = {
            0.5 + 1.0 * (double)(k % 4U), 0.5 + 1.4 * (double)(k / 4U), 0.0,
        };
        shots[k].position = p;
        shots[k].origin_time_known = (k % 2U) == 0U;
        shots[k].origin_time_us = 100000;
        truth_arrivals(p, 0.1, shots[k].arrival_time_us);
        for (size_t s = 0U; s < 8U; ++s) {
            shots[k].arrival_valid[s] = true;
        }
    }

    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &SLAB_MIN, &SLAB_MAX, 0.1, 0U, V_MPS) == ESP_OK);
    const localization_point3d_t hidden = { 2.3, 2.1, 0.0 };
    const double before_error = locate_slab(&model, hidden);

    localization_eikonal_calibration_config_t config = localization_eikonal_calibration_default_config();
    config.timing_uncertainty_us = SIGMA_US;
    config.min_velocity_mps = 50.0;
    config.max_velocity_mps = 600.0;
    config.max_iterations = 12U;
    localization_eikonal_calibration_result_t result;
    assert(localization_eikonal_calibrate(&model, SLAB_SENSORS, 8U, shots, 12U, &config, &result) == ESP_OK);

    double inside = 0.0, outside = 0.0;
    size_t n_inside = 0U, n_outside = 0U;
    const size_t count = localization_eikonal_node_count(&model);
    for (size_t i = 0U; i < count; ++i) {
        const localization_point3d_t p = localization_eikonal_node_position(&model, i);
        const double dx = p.x_m - 2.0, dy = p.y_m - 1.9;
        const double r = sqrt(dx * dx + dy * dy);
        if (r < 0.5) {
            inside += localization_eikonal_velocity(&model, i);
            ++n_inside;
        } else if (r > 1.2 && p.x_m > 0.3 && p.x_m < 3.7 && p.y_m > 0.3 && p.y_m < 3.5) {
            outside += localization_eikonal_velocity(&model, i);
            ++n_outside;
        }
    }
    inside /= (double)n_inside;
    outside /= (double)n_outside;
    const double after_error = locate_slab(&model, hidden);
    printf("  tomography: %zu rays, %zu updates, misfit %.0f -> %.0f us; slow zone %.0f m/s, surroundings %.0f m/s\n",
           result.rays_used, result.iterations, result.rms_before_us, result.rms_after_us, inside, outside);
    printf("  hidden source: homogeneous model %.3f m, calibrated model %.3f m\n", before_error, after_error);
    assert(result.rays_used == 96U);
    assert(result.iterations >= 2U);
    assert(result.rms_after_us < 0.4 * result.rms_before_us);
    assert(inside < 0.8 * V_MPS);
    assert(fabs(outside - V_MPS) < 0.15 * V_MPS);
    assert(result.nodes_updated > 0U && result.min_velocity_mps >= 50.0 && result.max_velocity_mps <= 600.0);
    assert(after_error < 0.5 * before_error);

    /* Invalid calibration input. */
    config.min_velocity_mps = 700.0;
    assert(localization_eikonal_calibrate(&model, SLAB_SENSORS, 8U, shots, 12U, &config, &result) == ESP_ERR_INVALID_ARG);
    config = localization_eikonal_calibration_default_config();
    config.timing_uncertainty_us = SIGMA_US;
    config.min_velocity_mps = 50.0;
    config.max_velocity_mps = 600.0;
    for (size_t s = 1U; s < 8U; ++s) {
        shots[0].arrival_valid[s] = false;
    }
    assert(localization_eikonal_calibrate(&model, SLAB_SENSORS, 8U, shots, 1U, &config, &result) == ESP_ERR_INVALID_STATE);
    localization_eikonal_model_free(&model);
}


static double air_above_surface(const localization_point3d_t *p, void *context)
{
    (void)context;
    return p->z_m > -0.05 ? 0.0 : V_MPS; /* the scan says: air from 5 cm below the pods up */
}


static void test_sensor_on_a_surface_next_to_air(void)
{
    /* A pod sits on the surface, so its own node can classify as air;
     * the field must still start from it (the first cell is not tested). */
    const localization_point3d_t min = { 0.0, 0.0, -1.0 };
    const localization_point3d_t max = { 2.0, 2.0, 0.3 };
    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &min, &max, 0.1, 0U, V_MPS) == ESP_OK);
    localization_eikonal_model_assign(&model, air_above_surface, NULL);
    const localization_point3d_t pod = { 1.0, 1.0, 0.0 };
    float *times = malloc(localization_eikonal_node_count(&model) * sizeof(float));
    assert(localization_eikonal_travel_times(&model, &pod, times) == ESP_OK);
    const localization_point3d_t buried = { 1.3, 0.6, -0.7 };
    const double t = localization_eikonal_time_at(&model, times, &buried);
    assert(isfinite(t));
    assert(fabs(t - ext_test_distance(pod, buried) / V_MPS) / t < 0.05);
    /* Air is never crossed: a point in it is reached only from the rock. */
    const localization_point3d_t in_air = { 1.5, 1.5, 0.2 };
    assert(isinf(localization_eikonal_time_at(&model, times, &in_air)));
    free(times);
    localization_eikonal_model_free(&model);
}


static void test_invalid_model_input(void)
{
    localization_eikonal_model_t model;
    const localization_point3d_t a = { 0.0, 0.0, 0.0 };
    const localization_point3d_t b = { 1.0, 1.0, 0.0 };
    assert(localization_eikonal_model_init(&model, &b, &a, 0.1, 0U, V_MPS) == ESP_ERR_INVALID_ARG);
    assert(localization_eikonal_model_init(&model, &a, &b, 0.0, 0U, V_MPS) == ESP_ERR_INVALID_ARG);
    assert(localization_eikonal_model_init(&model, &a, &b, 0.1, 0U, -1.0) == ESP_ERR_INVALID_ARG);
    /* A tight node budget enlarges the cell. */
    assert(localization_eikonal_model_init(&model, &a, &b, 0.01, 200U, V_MPS) == ESP_OK);
    assert(localization_eikonal_node_count(&model) <= 200U && model.cell_m > 0.01);
    localization_eikonal_model_free(&model);
    assert(model.slowness_s_per_m == NULL);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_homogeneous_accuracy();
    test_wave_goes_around_a_void();
    test_location_through_scan_voids();
    test_free_velocity_scale_and_robust_pick();
    test_calibration_hits_map_a_slow_zone();
    test_sensor_on_a_surface_next_to_air();
    test_invalid_model_input();
    printf("localization eikonal tests passed\n");
    return 0;
}
