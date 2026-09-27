#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_bayes.h"
#include "localization_calculate_policy.h"
#include "localization_eikonal.h"
#include "localization_psafuvs.h"
#include "localization_semblance.h"
#include "localization_suea.h"
#include "localization_tdcas.h"
#include "scan_occupancy.h"

/*
 * End to end: the empty (FREE) and filled (OCCUPIED) space the phone scan
 * sends to the ESP32 -> scan_occupancy_parse() -> the policy's scan bridges
 * -> TDCAS, PSAFUVS, SUEA, and the eikonal travel times shared by eikonal
 * location, eikonal-focused semblance and the Bayesian posterior. The
 * bytes are tests/vectors/
 * scan_occupancy_void_scene_v1.bin, written by the WebUI encoder
 * (webui/tests/xr/occupancyVoidSceneVector.test.ts), so a format drift on
 * either side fails here.
 *
 * Scene (sensor frame, 0.1 m voxels, 3 m x 3 m): observed surface skin
 * z in [-0.1, 0); open air z in [0, 0.2); a trench (a gap in the rubble)
 * x in [1.4, 1.7), all y, from z = -0.9 to the air; interior unknown. The
 * scan frame is rotated 90 degrees about Z and shifted (10, -5, 2).
 *
 * A buried source right of the trench, shallower than its bottom, is heard
 * by five pods on the same side and two on the far side, whose waves must
 * dive under the trench: straight-ray arrivals are wrong for exactly those
 * two.
 */

#define GEOMETRY_VERSION 424242U
#define VP 300.0
#define VS 160.0
#define EMISSION_S 1.0

static const double TRENCH_LOW_X = 1.4;
static const double TRENCH_HIGH_X = 1.7;
static const double TRENCH_BOTTOM_Z = -0.9;

/* Beside the trench and shallower than its bottom: far-side waves must
 * dive under the trench and come back up (a ~0.5 m detour). */
static const localization_point3d_t SOURCE = { 2.3, 1.6, -0.6 };

/* Five pods right of the trench, then two on its far side. */
static const localization_point3d_t PODS[7] = {
    { 2.1, 0.3, -0.03 }, { 2.8, 0.6, -0.03 }, { 2.6, 2.7, -0.03 }, { 2.0, 2.2, -0.03 },
    { 2.9, 1.5, -0.03 }, { 0.4, 0.5, -0.03 }, { 0.6, 2.6, -0.03 },
};
#define FAR_SIDE_MASK ((1U << 5) | (1U << 6))


static double distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


/* True shortest path: the trench is invariant along y, so the path's x-z
 * projection bends under the trench bottom corners and y varies linearly. */
static double true_path(localization_point3d_t sensor, localization_point3d_t source)
{
    if ((sensor.x_m < TRENCH_LOW_X) == (source.x_m < TRENCH_LOW_X)) {
        return distance(sensor, source);
    }
    const localization_point3d_t a = sensor.x_m < source.x_m ? sensor : source;
    const localization_point3d_t b = sensor.x_m < source.x_m ? source : sensor;
    const double z_low = a.z_m + (b.z_m - a.z_m) * (TRENCH_LOW_X - a.x_m) / (b.x_m - a.x_m);
    const double z_high = a.z_m + (b.z_m - a.z_m) * (TRENCH_HIGH_X - a.x_m) / (b.x_m - a.x_m);
    double xz = 0.0;
    if (z_low <= TRENCH_BOTTOM_Z && z_high <= TRENCH_BOTTOM_Z) {
        xz = sqrt((b.x_m - a.x_m) * (b.x_m - a.x_m) + (b.z_m - a.z_m) * (b.z_m - a.z_m));
    } else {
        const double to_corner = hypot(TRENCH_LOW_X - a.x_m, TRENCH_BOTTOM_Z - a.z_m);
        /* b below the bottom level sees the near corner directly. */
        xz = b.z_m <= TRENCH_BOTTOM_Z
            ? to_corner + hypot(b.x_m - TRENCH_LOW_X, b.z_m - TRENCH_BOTTOM_Z)
            : to_corner + (TRENCH_HIGH_X - TRENCH_LOW_X) + hypot(b.x_m - TRENCH_HIGH_X, b.z_m - TRENCH_BOTTOM_Z);
    }
    const double dy = b.y_m - a.y_m;
    return sqrt(xz * xz + dy * dy);
}


static uint8_t *read_vector(size_t *length)
{
    static const char *const candidates[] = {
        "tests/vectors/scan_occupancy_void_scene_v1.bin",
        "../../../tests/vectors/scan_occupancy_void_scene_v1.bin",
    };
    for (size_t i = 0U; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        FILE *file = fopen(candidates[i], "rb");
        if (file == NULL) {
            continue;
        }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        uint8_t *data = malloc((size_t)size);
        assert(data != NULL);
        assert(fread(data, 1U, (size_t)size, file) == (size_t)size);
        fclose(file);
        *length = (size_t)size;
        return data;
    }
    assert(!"vector tests/vectors/scan_occupancy_void_scene_v1.bin not found");
    return NULL;
}


static void load_scene(scan_occupancy_t *occupancy)
{
    size_t length = 0U;
    uint8_t *data = read_vector(&length);
    assert(scan_occupancy_parse(data, length, occupancy) == ESP_OK);
    free(data);
}


static scan_occupancy_class_t classify_sensor_point(const scan_occupancy_t *occupancy, double x, double y, double z)
{
    const double sensor[3] = { x, y, z };
    double scan[3];
    scan_occupancy_sensor_to_scan(occupancy, sensor, scan);
    return scan_occupancy_classify(occupancy, scan);
}


static void test_scan_sectors_arrive_intact_and_registered(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    assert(strcmp(occupancy.frame_id, "scan-crc32:0badc0de") == 0);
    assert(occupancy.free_count == 30U * 30U * 2U + 3U * 30U * 9U);
    assert(occupancy.occupied_count == 30U * 30U - 3U * 30U);
    assert(occupancy.voxel_size_m == 0.1 && occupancy.below_surface_m == 2.0);
    assert(occupancy.registration.present);
    assert(strcmp(occupancy.registration.source, "apriltag") == 0);

    /* Bound to exactly this frozen geometry. */
    assert(scan_occupancy_registration_usable(&occupancy, GEOMETRY_VERSION));
    assert(!scan_occupancy_registration_usable(&occupancy, GEOMETRY_VERSION + 1U));

    /* Sensor-frame points land in the right sectors only through the
     * rotated registration. */
    assert(classify_sensor_point(&occupancy, 1.55, 1.55, -0.55) == SCAN_OCCUPANCY_FREE);     /* trench */
    assert(classify_sensor_point(&occupancy, 0.55, 0.55, 0.15) == SCAN_OCCUPANCY_FREE);      /* open air */
    assert(classify_sensor_point(&occupancy, 0.55, 0.55, -0.05) == SCAN_OCCUPANCY_OCCUPIED); /* surface */
    assert(classify_sensor_point(&occupancy, 0.55, 0.55, -0.55) == SCAN_OCCUPANCY_UNKNOWN);  /* interior */
    assert(classify_sensor_point(&occupancy, 0.55, 0.55, -2.5) == SCAN_OCCUPANCY_OUTSIDE);   /* too deep */
    assert(classify_sensor_point(&occupancy, 0.55, 0.55, 1.0) == SCAN_OCCUPANCY_OUTSIDE);    /* high air */

    /* ... and through the policy bridges. */
    const localization_point3d_t trench = { 1.55, 1.55, -0.55 };
    const localization_point3d_t interior = { 0.55, 0.55, -0.55 };
    const localization_point3d_t surface = { 0.55, 0.55, -0.05 };
    const localization_point3d_t high_air = { 0.55, 0.55, 1.0 };
    assert(!localization_calculate_scan_candidate_allowed(&trench, &occupancy));
    assert(localization_calculate_scan_candidate_allowed(&interior, &occupancy));
    assert(localization_calculate_scan_suea_cell(&trench, &occupancy) == LOCALIZATION_SUEA_CELL_EMPTY);
    assert(localization_calculate_scan_suea_cell(&surface, &occupancy) == LOCALIZATION_SUEA_CELL_SOLID);
    assert(localization_calculate_scan_suea_cell(&interior, &occupancy) == LOCALIZATION_SUEA_CELL_UNKNOWN);
    assert(localization_calculate_scan_suea_cell(&high_air, &occupancy) == LOCALIZATION_SUEA_CELL_EMPTY);

    const localization_calculate_scan_ray_context_t ray = {
        .occupancy = &occupancy,
        .sensor_clearance_m = LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS * occupancy.voxel_size_m,
    };
    for (size_t i = 0U; i < 7U; ++i) {
        const bool clear = localization_calculate_scan_ray_clear(&PODS[i], &SOURCE, (void *)&ray);
        assert(clear == ((FAR_SIDE_MASK & (1U << i)) == 0U));
    }
    scan_occupancy_free(&occupancy);
}


static void test_tdcas_with_scan_voids(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    localization_observation_t observations[7];
    for (size_t i = 0U; i < 7U; ++i) {
        observations[i].sensor_position = PODS[i];
        observations[i].arrival_time_us = (int64_t)llround((EMISSION_S + true_path(PODS[i], SOURCE) / VP) * 1.0e6);
    }

    localization_tdcas_result_t unconstrained;
    assert(localization_tdcas_solve(observations, 7U, NULL, &unconstrained) == ESP_OK);

    const localization_calculate_scan_ray_context_t ray = {
        .occupancy = &occupancy,
        .sensor_clearance_m = LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS * occupancy.voxel_size_m,
    };
    const localization_tdcas_config_t config = {
        .candidate_filter = { .allowed = localization_calculate_scan_candidate_allowed, .context = &occupancy },
        .ray_filter = { .clear = localization_calculate_scan_ray_clear, .context = (void *)&ray },
    };
    localization_tdcas_result_t constrained;
    assert(localization_tdcas_solve(observations, 7U, &config, &constrained) == ESP_OK);
    assert(constrained.status == LOCALIZATION_TDCAS_SOLVED);
    assert(constrained.planar_layout);
    /* Exactly the two far-side pods are dropped, with every combination
     * containing them (C(7,5) - C(5,5) = 20 of 21 when all are valid). */
    assert(constrained.sensor_mask_obstructed == FAR_SIDE_MASK);
    assert(constrained.sensors_used == 5U);
    assert(constrained.combinations_filtered > 0U);
    assert(localization_calculate_scan_candidate_allowed(&constrained.position, &occupancy));

    const double before = unconstrained.status == LOCALIZATION_TDCAS_SOLVED
        ? distance(unconstrained.position, SOURCE) : INFINITY;
    const double after = distance(constrained.position, SOURCE);
    assert(after < 0.05);
    assert(before > 3.0 * after);
    printf("[SCAN->ESP32] TDCAS: straight rays error=%.3f m, scan voids error=%.4f m (%zu of %zu combinations dropped)\n",
           before, after, constrained.combinations_filtered, constrained.combinations_total);
    scan_occupancy_free(&occupancy);
}


static void test_psafuvs_with_scan_voids(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    localization_psafuvs_observation_t observations[7];
    for (size_t i = 0U; i < 7U; ++i) {
        const double path = true_path(PODS[i], SOURCE);
        observations[i].sensor_position = PODS[i];
        observations[i].p_arrival_time_us = (int64_t)llround((EMISSION_S + path / VP) * 1.0e6);
        observations[i].s_arrival_time_us = (int64_t)llround((EMISSION_S + path / VS) * 1.0e6);
    }

    localization_psafuvs_result_t unconstrained;
    assert(localization_psafuvs_solve(observations, 7U, NULL, &unconstrained) == ESP_OK);

    const localization_calculate_scan_ray_context_t ray = {
        .occupancy = &occupancy,
        .sensor_clearance_m = LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS * occupancy.voxel_size_m,
    };
    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
    config.candidate_filter.context = &occupancy;
    config.ray_filter.clear = localization_calculate_scan_ray_clear;
    config.ray_filter.context = (void *)&ray;
    localization_psafuvs_result_t constrained;
    assert(localization_psafuvs_solve(observations, 7U, &config, &constrained) == ESP_OK);
    assert(constrained.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(constrained.sensor_mask_obstructed == FAR_SIDE_MASK);
    assert(constrained.sensors_used == 5U);

    const double before = unconstrained.status == LOCALIZATION_PSAFUVS_SOLVED
        ? distance(unconstrained.position, SOURCE) : INFINITY;
    const double after = distance(constrained.position, SOURCE);
    assert(after < 0.02);
    assert(before > 3.0 * after);
    assert(fabs(constrained.p_velocity_mps - VP) < 3.0 && fabs(constrained.s_velocity_mps - VS) < 2.0);
    printf("[SCAN->ESP32] PSAFUVS: straight rays error=%.3f m, scan voids error=%.4f m, vP=%.1f vS=%.1f\n",
           before, after, constrained.p_velocity_mps, constrained.s_velocity_mps);
    scan_occupancy_free(&occupancy);
}


static void test_suea_travels_around_scanned_voids(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);

    /* The pipeline's grid: the traveltime search box, one step wider/higher. */
    const localization_traveltime_search_bounds_t search = {
        .min_corner = { 0.0, 0.0, -2.0 },
        .max_corner = { 3.0, 3.0, -0.03 },
        .step_m = 0.1,
    };
    localization_point3d_t lo, hi;
    localization_calculate_suea_grid_bounds(&search, &lo, &hi);

    localization_suea_pick_t picks[7];
    for (size_t i = 0U; i < 7U; ++i) {
        picks[i].sensor_position = PODS[i];
        picks[i].arrival_time_us = (int64_t)llround((EMISSION_S + true_path(PODS[i], SOURCE) / VP) * 1.0e6);
        picks[i].weight = 1.0;
    }
    /* The caller's velocity as a prior with its uncertainty, as in
     * localization_calculate_run(). */
    localization_suea_config_t config = {
        .timing_uncertainty_us = 20.0,
        .prior_velocity_mps = VP * 1.02,
        .prior_velocity_sigma_mps = 15.0,
        .refine_divisor = 4U,
    };

    localization_suea_grid_t straight_grid;
    assert(localization_suea_grid_init(&straight_grid, &lo, &hi, search.step_m, 0U) == ESP_OK);
    localization_suea_result_t straight;
    assert(localization_suea_locate(&straight_grid, picks, 7U, &config, &straight) == ESP_OK);
    assert(straight.status == LOCALIZATION_SUEA_SOLVED);

    localization_suea_grid_t scan_grid;
    assert(localization_suea_grid_init(&scan_grid, &lo, &hi, search.step_m, 0U) == ESP_OK);
    localization_suea_grid_classify(&scan_grid, localization_calculate_scan_suea_cell, &occupancy);
    assert(localization_suea_grid_count(&scan_grid, LOCALIZATION_SUEA_CELL_EMPTY) > 0U);
    assert(localization_suea_grid_count(&scan_grid, LOCALIZATION_SUEA_CELL_SOLID) > 0U);
    config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
    config.candidate_filter.context = &occupancy;
    localization_suea_result_t with_scan;
    assert(localization_suea_locate(&scan_grid, picks, 7U, &config, &with_scan) == ESP_OK);
    assert(with_scan.status == LOCALIZATION_SUEA_SOLVED);
    assert(with_scan.pick_mask_unreachable == 0U); /* every pod snapped onto the surface */
    assert(with_scan.max_detour_m > 0.05);
    assert(localization_calculate_scan_candidate_allowed(&with_scan.best_position, &occupancy));

    /* Resolution-limited: at the pipeline's 0.1 m cells the 0.3 m trench is
     * classified up to half a cell wider and deeper, and any-angle paths
     * pivot on cell centers rather than the true corner (far-pod paths come
     * out ~3 % long). The scan still halves the error and explains the
     * arrivals several times better than straight rays. */
    const double before = distance(straight.best_position, SOURCE);
    const double after = distance(with_scan.best_position, SOURCE);
    assert(after < 0.5 * before);
    assert(after < 0.3);
    assert(with_scan.best_residual_rms_us < 0.4 * straight.best_residual_rms_us);
    printf("[SCAN->ESP32] SUEA: straight rays error=%.3f m (rms %.0f us), scan voids error=%.3f m (rms %.0f us), detour %.2f m\n",
           before, straight.best_residual_rms_us, after, with_scan.best_residual_rms_us, with_scan.max_detour_m);

    localization_suea_grid_free(&straight_grid);
    localization_suea_grid_free(&scan_grid);
    scan_occupancy_free(&occupancy);
}


/* The pipeline's eikonal model over the SUEA box, homogeneous or with the
 * scan's voids impassable; fields for the seven pods. */
static void eikonal_scene(
    const scan_occupancy_t *occupancy,
    localization_eikonal_model_t *model,
    localization_eikonal_fields_t *fields
)
{
    const localization_traveltime_search_bounds_t search = {
        .min_corner = { 0.0, 0.0, -2.0 },
        .max_corner = { 3.0, 3.0, -0.03 },
        .step_m = 0.1,
    };
    localization_point3d_t lo, hi;
    localization_calculate_suea_grid_bounds(&search, &lo, &hi);
    assert(localization_eikonal_model_init(model, &lo, &hi, search.step_m, 40000U, VP) == ESP_OK);
    if (occupancy != NULL) {
        const localization_calculate_scan_velocity_context_t scan = { occupancy, VP, 0.5 * model->cell_m, false };
        localization_eikonal_model_assign(model, localization_calculate_scan_eikonal_velocity, (void *)&scan);
        assert(localization_eikonal_passable_count(model) < localization_eikonal_node_count(model));
    }
    assert(localization_eikonal_fields_compute(model, PODS, 7U, fields) == ESP_OK);
}


static void test_eikonal_travels_around_scanned_voids(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    localization_eikonal_pick_t picks[7];
    for (size_t i = 0U; i < 7U; ++i) {
        picks[i].arrival_time_us = (int64_t)llround((EMISSION_S + true_path(PODS[i], SOURCE) / VP) * 1.0e6);
        picks[i].weight = 1.0;
    }
    /* The scan's trench is impassable where a whole 0.1 m cell is empty;
     * a node on its wall stays passable (the wall is not widened). */
    const localization_calculate_scan_velocity_context_t scan = { &occupancy, VP, 0.05, false };
    const localization_point3d_t trench = { 1.55, 1.55, -0.55 };
    const localization_point3d_t wall = { 1.4, 1.55, -0.55 };
    const localization_point3d_t interior = { 0.55, 0.55, -0.55 };
    assert(localization_calculate_scan_eikonal_velocity(&trench, (void *)&scan) == 0.0);
    assert(localization_calculate_scan_eikonal_velocity(&wall, (void *)&scan) == VP);
    assert(localization_calculate_scan_eikonal_velocity(&interior, (void *)&scan) == VP);
    const localization_calculate_scan_velocity_context_t point_only = { &occupancy, VP, 0.0, false };
    assert(localization_calculate_scan_eikonal_velocity(&wall, (void *)&point_only) == 0.0);

    localization_eikonal_locate_config_t config = {
        .timing_uncertainty_us = 20.0,
        .velocity_scale_sigma = 0.05,
        .refine_divisor = 4U,
    };
    localization_eikonal_model_t straight_model;
    localization_eikonal_fields_t straight_fields;
    eikonal_scene(NULL, &straight_model, &straight_fields);
    localization_eikonal_result_t straight;
    assert(localization_eikonal_locate(&straight_model, &straight_fields, picks, 7U, &config, &straight) == ESP_OK);

    localization_eikonal_model_t scan_model;
    localization_eikonal_fields_t scan_fields;
    eikonal_scene(&occupancy, &scan_model, &scan_fields);
    config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
    config.candidate_filter.context = &occupancy;
    localization_eikonal_result_t with_scan;
    assert(localization_eikonal_locate(&scan_model, &scan_fields, picks, 7U, &config, &with_scan) == ESP_OK);
    assert(with_scan.status == LOCALIZATION_EIKONAL_SOLVED);
    assert(with_scan.pick_mask_unreachable == 0U && with_scan.pick_mask_rejected == 0U);
    assert(localization_calculate_scan_candidate_allowed(&with_scan.position, &occupancy));

    const double before = distance(straight.position, SOURCE);
    const double after = distance(with_scan.position, SOURCE);
    printf("[SCAN->ESP32] eikonal: straight rays error=%.3f m (rms %.0f us), scan voids error=%.3f m (rms %.0f us)\n",
           before, straight.rms_residual_us, after, with_scan.rms_residual_us);
    assert(after < 0.2);
    assert(after < 0.5 * before);
    assert(with_scan.rms_residual_us < 0.5 * straight.rms_residual_us);

    localization_eikonal_fields_free(&straight_fields);
    localization_eikonal_model_free(&straight_model);
    localization_eikonal_fields_free(&scan_fields);
    localization_eikonal_model_free(&scan_model);
    scan_occupancy_free(&occupancy);
}


static void test_bayesian_region_with_scanned_voids(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    localization_bayes_pick_t picks[7];
    for (size_t i = 0U; i < 7U; ++i) {
        picks[i].sensor_position = PODS[i];
        picks[i].arrival_time_us = (int64_t)llround((EMISSION_S + true_path(PODS[i], SOURCE) / VP) * 1.0e6);
        picks[i].weight = 1.0;
    }
    const localization_extended_bounds_t bounds = localization_calculate_extended_bounds(
        &(localization_traveltime_search_bounds_t) { { 0.0, 0.0, -2.0 }, { 3.0, 3.0, -0.03 }, 0.1 }
    );
    /* The pipeline's settings: Student-t, velocity +-10 m/s, a 20 us pick
     * sigma, the misfit check, budgets. */
    localization_bayes_config_t config = {
        .velocity_mps = VP,
        .velocity_sigma_mps = 10.0,
        .timing_uncertainty_us = 20.0,
        .likelihood = LOCALIZATION_BAYES_STUDENT_T,
        .velocity_nodes = 5U,
        .max_cells = 8000U,
        .refine_divisor = 4U,
        .inflate_on_misfit = true,
    };
    localization_bayes_result_t straight;
    assert(localization_bayes_locate(picks, 7U, &bounds, &config, &straight) == ESP_OK);

    localization_eikonal_model_t model;
    localization_eikonal_fields_t fields;
    eikonal_scene(&occupancy, &model, &fields);
    localization_eikonal_travel_time_context_t times = { &model, &fields };
    config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
    config.candidate_filter.context = &occupancy;
    config.travel_time_model.travel_time = localization_eikonal_travel_time;
    config.travel_time_model.context = &times;
    /* ... plus the eikonal model's own error, as in the pipeline. */
    config.model_error_relative = 0.015;
    config.model_error_floor_us = localization_calculate_discretization_error_us(
        fmax(model.cell_m, occupancy.voxel_size_m), VP
    );
    localization_bayes_result_t with_scan;
    assert(localization_bayes_locate(picks, 7U, &bounds, &config, &with_scan) == ESP_OK);
    assert(with_scan.status == LOCALIZATION_BAYES_SOLVED);

    const double before = distance(straight.map_position, SOURCE);
    const double after = distance(with_scan.map_position, SOURCE);
    printf("[SCAN->ESP32] Bayesian: straight MAP error=%.3f m, scan voids MAP error=%.3f m, "
           "sd (%.2f, %.2f, %.2f) m, 90%% volume %.4f m^3, chi2 %.1f/%d, sigma x%.1f; straight chi2 %.1f -> x%.1f\n",
           before, after, with_scan.std_m.x_m, with_scan.std_m.y_m, with_scan.std_m.z_m,
           with_scan.credible_volume_m3, with_scan.chi_square, with_scan.chi_square_dof, with_scan.sigma_inflation,
           straight.chi_square, straight.sigma_inflation);
    /* Straight rays cannot explain the far pods: the check widens that
     * region instead of reporting false confidence. */
    assert(straight.sigma_inflation > 1.0);
    assert(after < 0.2 && after < 0.5 * before);
    /* The truth lies in the 90% region (within the lattice's own cell). */
    assert(SOURCE.x_m >= with_scan.credible_min.x_m - with_scan.cell_m && SOURCE.x_m <= with_scan.credible_max.x_m + with_scan.cell_m);
    assert(SOURCE.y_m >= with_scan.credible_min.y_m - with_scan.cell_m && SOURCE.y_m <= with_scan.credible_max.y_m + with_scan.cell_m);
    assert(SOURCE.z_m >= with_scan.credible_min.z_m - with_scan.cell_m && SOURCE.z_m <= with_scan.credible_max.z_m + with_scan.cell_m);

    /* ... and the record carries the region. */
    const localization_calculate_run_context_t context = {
        .object_id = "OBJ_0001", .session_id = "S000001", .computed_at_ms = 5U,
        .geometry_computed_at_ms = GEOMETRY_VERSION, .dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
        .propagation_model_name = "tap", .propagation_speed_mps = VP, .propagation_speed_uncertainty_mps = 10.0,
    };
    localization_run_record_t record;
    localization_calculate_bayes_to_run_record(&context, &with_scan, "bayesian_scan_voids", NULL,
                                               LOCALIZATION_FAILURE_CLASS_INTERNAL, true,
                                               0x7FU, 0U, 1U, 60.0, &record);
    assert(record.solved && strcmp(record.method_name, "bayesian_scan_voids") == 0);
    assert(record.uncertainty.credible_level == 0.9 && record.uncertainty.credible_volume_m3 > 0.0);
    assert(record.estimated_p_velocity_mps > 0.0);

    localization_eikonal_fields_free(&fields);
    localization_eikonal_model_free(&model);
    scan_occupancy_free(&occupancy);
}


/* Records of a zero-phase wavelet arriving along the true paths. */
#define SEMBLANCE_RATE_HZ 2000U
#define SEMBLANCE_SAMPLES 2400U
static float s_records[7][SEMBLANCE_SAMPLES];

static void test_semblance_focuses_with_eikonal_times(void)
{
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    localization_semblance_channel_t channels[7];
    size_t reference = SIZE_MAX;
    for (size_t i = 0U; i < 7U; ++i) {
        const double arrival_s = 0.1 + true_path(PODS[i], SOURCE) / VP;
        for (size_t n = 0U; n < SEMBLANCE_SAMPLES; ++n) {
            const double a = 3.141592653589793 * 60.0 * ((double)n / SEMBLANCE_RATE_HZ - arrival_s);
            s_records[i][n] = (float)((1.0 - 2.0 * a * a) * exp(-a * a));
        }
        const size_t index = (size_t)llround(arrival_s * SEMBLANCE_RATE_HZ);
        reference = index < reference ? index : reference;
        channels[i] = (localization_semblance_channel_t) {
            .samples = s_records[i], .sample_count = SEMBLANCE_SAMPLES,
            .sample_rate_hz = SEMBLANCE_RATE_HZ, .sensor_position = PODS[i],
        };
    }
    const localization_semblance_search_bounds_t bounds = { { 0.0, 0.0, -2.0 }, { 3.0, 3.0, -0.03 }, 0.1 };
    localization_semblance_config_t config = {
        .propagation_speed_mps = VP,
        .reference_sample_index = reference,
        .window_half_span_samples = 8U,
        .refine_divisor = 4U,
    };
    config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
    config.candidate_filter.context = &occupancy;
    localization_semblance_result_t straight;
    assert(localization_semblance_search(channels, 7U, &bounds, &config, &straight) == ESP_OK);

    localization_eikonal_model_t model;
    localization_eikonal_fields_t fields;
    eikonal_scene(&occupancy, &model, &fields);
    localization_eikonal_travel_time_context_t times = { &model, &fields };
    config.travel_time_model.travel_time = localization_eikonal_travel_time;
    config.travel_time_model.context = &times;
    localization_semblance_result_t focused;
    assert(localization_semblance_search(channels, 7U, &bounds, &config, &focused) == ESP_OK);
    assert(focused.found);
    const double before = distance(straight.best_position, SOURCE);
    const double after = distance(focused.best_position, SOURCE);
    printf("[SCAN->ESP32] semblance: straight rays error=%.3f m (semblance %.2f), eikonal times error=%.3f m (semblance %.2f)\n",
           before, straight.best_semblance, after, focused.best_semblance);
    assert(focused.best_semblance > straight.best_semblance);
    assert(after < 0.25);
    assert(after <= before);

    localization_eikonal_fields_free(&fields);
    localization_eikonal_model_free(&model);
    scan_occupancy_free(&occupancy);
}


static void test_stale_registration_never_constrains(void)
{
    /* Same bytes, but the session geometry was recomputed afterwards: the
     * pipeline must treat the sectors as unusable (fail closed). */
    scan_occupancy_t occupancy;
    load_scene(&occupancy);
    assert(!scan_occupancy_registration_usable(&occupancy, 999U));
    occupancy.registration.present = false;
    assert(!scan_occupancy_registration_usable(&occupancy, GEOMETRY_VERSION));
    scan_occupancy_free(&occupancy);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_scan_sectors_arrive_intact_and_registered();
    test_tdcas_with_scan_voids();
    test_psafuvs_with_scan_voids();
    test_suea_travels_around_scanned_voids();
    test_eikonal_travels_around_scanned_voids();
    test_bayesian_region_with_scanned_voids();
    test_semblance_focuses_with_eikonal_times();
    test_stale_registration_never_constrains();
    printf("localization scan->ESP32 method tests passed\n");
    return 0;
}
