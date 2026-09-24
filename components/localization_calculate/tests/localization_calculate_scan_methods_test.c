#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_calculate_policy.h"
#include "localization_psafuvs.h"
#include "localization_suea.h"
#include "localization_tdcas.h"
#include "scan_occupancy.h"

/*
 * End to end: the empty (FREE) and filled (OCCUPIED) space the phone scan
 * sends to the ESP32 -> scan_occupancy_parse() -> the policy's scan bridges
 * -> TDCAS, PSAFUVS and SUEA. The bytes are tests/vectors/
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
     * out 3-5 % long). The scan still halves the error and explains the
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
    test_scan_sectors_arrive_intact_and_registered();
    test_tdcas_with_scan_voids();
    test_psafuvs_with_scan_voids();
    test_suea_travels_around_scanned_voids();
    test_stale_registration_never_constrains();
    printf("localization scan->ESP32 method tests passed\n");
    return 0;
}
