#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization.h"
#include "localization_suea.h"

/*
 * SUEA (Dong et al., IJMST 2022): empty-area-aware, velocity-free location
 * on a SOLID/UNKNOWN/EMPTY voxel grid. Synthetic ground truth: analytic
 * detours around thin walls and trenches, or a truth grid for voids.
 */

static double distance(localization_point3d_t a, localization_point3d_t b)
{
    const double dx = a.x_m - b.x_m;
    const double dy = a.y_m - b.y_m;
    const double dz = a.z_m - b.z_m;
    return sqrt(dx * dx + dy * dy + dz * dz);
}


static void make_grid(
    localization_suea_grid_t *grid,
    localization_point3d_t min_corner,
    localization_point3d_t max_corner,
    double cell_m
)
{
    assert(localization_suea_grid_init(grid, &min_corner, &max_corner, cell_m, 200000U) == ESP_OK);
}


/* Marks every cell whose center lies in the axis-aligned box EMPTY. */
static void carve_empty(
    localization_suea_grid_t *grid,
    localization_point3d_t low,
    localization_point3d_t high
)
{
    for (size_t iz = 0U; iz < grid->nz; ++iz) {
        for (size_t iy = 0U; iy < grid->ny; ++iy) {
            for (size_t ix = 0U; ix < grid->nx; ++ix) {
                const localization_point3d_t c = localization_suea_grid_center(grid, ix, iy, iz);
                if (
                    c.x_m >= low.x_m && c.x_m <= high.x_m &&
                    c.y_m >= low.y_m && c.y_m <= high.y_m &&
                    c.z_m >= low.z_m && c.z_m <= high.z_m
                ) {
                    localization_suea_grid_set(grid, ix, iy, iz, LOCALIZATION_SUEA_CELL_EMPTY);
                }
            }
        }
    }
}


static localization_suea_config_t default_config(void)
{
    localization_suea_config_t config;
    memset(&config, 0, sizeof(config));
    config.timing_uncertainty_us = 20.0;
    config.refine_divisor = 4U;
    return config;
}


static void test_grid_geometry_and_budget(void)
{
    localization_suea_grid_t grid;
    make_grid(&grid, (localization_point3d_t) { 0.0, 0.0, 0.0 }, (localization_point3d_t) { 2.0, 1.0, 0.0 }, 0.1);
    assert(grid.nx == 21U && grid.ny == 11U && grid.nz == 1U);
    assert(fabs(grid.cell_m - 0.1) < 1.0e-12);
    /* A flat box puts the single layer's centers exactly on its z. */
    const localization_point3d_t c = localization_suea_grid_center(&grid, 0U, 0U, 0U);
    assert(fabs(c.z_m) < 1.0e-12 && fabs(c.x_m) < 1.0e-12 && fabs(c.y_m) < 1.0e-12);
    assert(localization_suea_grid_count(&grid, LOCALIZATION_SUEA_CELL_UNKNOWN) == 231U);
    size_t ix = 0U, iy = 0U, iz = 0U;
    const localization_point3d_t inside = { 1.02, 0.49, 0.0 };
    assert(localization_suea_grid_locate(&grid, &inside, &ix, &iy, &iz));
    assert(ix == 10U && iy == 5U && iz == 0U);
    const localization_point3d_t outside = { 3.0, 0.5, 0.0 };
    assert(!localization_suea_grid_locate(&grid, &outside, &ix, &iy, &iz));
    localization_suea_grid_free(&grid);

    /* The cell budget enlarges the cell by an integer factor. */
    const localization_point3d_t lo = { 0.0, 0.0, -2.0 };
    const localization_point3d_t hi = { 4.0, 4.0, 0.0 };
    assert(localization_suea_grid_init(&grid, &lo, &hi, 0.05, 1000U) == ESP_OK);
    assert(localization_suea_grid_cell_count(&grid) <= 1000U);
    assert(grid.cell_m > 0.05);
    assert(fabs(grid.cell_m / 0.05 - round(grid.cell_m / 0.05)) < 1.0e-9);
    localization_suea_grid_free(&grid);

    assert(localization_suea_grid_init(&grid, &hi, &lo, 0.1, 0U) == ESP_ERR_INVALID_ARG);
    assert(localization_suea_grid_init(&grid, &lo, &hi, 0.0, 0U) == ESP_ERR_INVALID_ARG);
}


static void test_unobstructed_field_is_exact_straight_distance(void)
{
    localization_suea_grid_t grid;
    make_grid(&grid, (localization_point3d_t) { 0.0, 0.0, -1.0 }, (localization_point3d_t) { 2.0, 2.0, 0.0 }, 0.1);
    const localization_point3d_t source = { 0.33, 1.71, -0.02 };
    localization_suea_field_t field;
    assert(localization_suea_field_compute(&grid, &source, &field) == ESP_OK);
    double worst = 0.0;
    for (size_t i = 0U; i < field.count; ++i) {
        size_t ix = i % grid.nx;
        size_t iy = (i / grid.nx) % grid.ny;
        size_t iz = i / (grid.nx * grid.ny);
        const double expected = distance(source, localization_suea_grid_center(&grid, ix, iy, iz));
        const double error = fabs((double)field.length_m[i] - expected);
        if (error > worst) worst = error;
    }
    /* Any-angle paths: no 26-direction staircase error at all. */
    assert(worst < 1.0e-5);
    const localization_point3d_t off_grid = { 1.234, 0.456, -0.789 };
    assert(fabs(localization_suea_field_length_at(&grid, &field, &off_grid) - distance(source, off_grid)) < 1.0e-9);
    localization_suea_field_free(&field);
    localization_suea_grid_free(&grid);
}


static void test_path_detours_around_a_wall(void)
{
    localization_suea_grid_t grid;
    make_grid(&grid, (localization_point3d_t) { 0.0, 0.0, 0.0 }, (localization_point3d_t) { 4.0, 4.0, 0.0 }, 0.05);
    /* A thin wall at x = 2 from the grid edge up to y = 3. */
    carve_empty(&grid, (localization_point3d_t) { 1.99, -1.0, -1.0 }, (localization_point3d_t) { 2.01, 3.001, 1.0 });
    const localization_point3d_t source = { 1.0, 1.0, 0.0 };
    const localization_point3d_t target = { 3.0, 1.0, 0.0 };
    localization_suea_field_t field;
    assert(localization_suea_field_compute(&grid, &source, &field) == ESP_OK);

    /* Around the wall's top: corners at (1.975, 3.025) and (2.025, 3.025). */
    const double analytic =
        distance(source, (localization_point3d_t) { 1.975, 3.025, 0.0 }) + 0.05 +
        distance((localization_point3d_t) { 2.025, 3.025, 0.0 }, target);
    const double length = localization_suea_field_length_at(&grid, &field, &target);
    assert(length >= analytic - 1.0e-6);
    assert(length <= analytic + 2.0 * grid.cell_m);
    assert(length > 2.0 * distance(source, target));
    /* Wall cells themselves are unreachable. */
    const localization_point3d_t in_wall = { 2.0, 1.0, 0.0 };
    assert(isinf(localization_suea_field_length_at(&grid, &field, &in_wall)));
    printf("[SUEA] wall detour: path=%.4f m analytic=%.4f m straight=%.4f m\n",
           length, analytic, distance(source, target));
    localization_suea_field_free(&field);
    localization_suea_grid_free(&grid);
}


static void make_picks(
    const localization_point3d_t *sensors,
    const double *path_m,
    size_t count,
    double emission_s,
    double v,
    localization_suea_pick_t *picks
)
{
    for (size_t i = 0U; i < count; ++i) {
        picks[i].sensor_position = sensors[i];
        picks[i].arrival_time_us = (int64_t)llround((emission_s + path_m[i] / v) * 1.0e6);
        picks[i].weight = 1.0;
    }
}


static void test_velocity_free_location_in_homogeneous_medium(void)
{
    localization_suea_grid_t grid;
    make_grid(&grid, (localization_point3d_t) { 0.0, 0.0, 0.0 }, (localization_point3d_t) { 4.0, 4.0, 0.0 }, 0.05);
    const localization_point3d_t sensors[5] = {
        { 0.2, 0.2, 0.0 }, { 3.8, 0.3, 0.0 }, { 0.1, 3.7, 0.0 }, { 3.9, 3.9, 0.0 }, { 2.2, 0.1, 0.0 },
    };
    const localization_point3d_t source = { 1.37, 2.61, 0.0 };
    double paths[5];
    for (size_t i = 0U; i < 5U; ++i) paths[i] = distance(sensors[i], source);
    localization_suea_pick_t picks[5];
    make_picks(sensors, paths, 5U, 4.0, 265.0, picks);

    localization_suea_config_t config = default_config();
    localization_suea_result_t result;
    assert(localization_suea_locate(&grid, picks, 5U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_SUEA_SOLVED);
    assert(result.picks_used_count == 5U && result.pick_mask_used == 0x1FU);
    assert(distance(result.best_position, source) < 0.02);
    assert(fabs(result.velocity_mps - 265.0) < 5.0);
    assert(llabs(result.emission_time_us - 4000000) < 200);
    assert(result.max_detour_m < 1.0e-6);
    assert(result.empty_cell_count == 0U);
    localization_suea_grid_free(&grid);
}


/* Thin wall at x = 2 (x in [1.975, 2.025]) from the bottom edge up to
 * y = 2.625: exact shortest path around its free top end. */
static double wall_path(localization_point3d_t s, localization_point3d_t t)
{
    const bool s_left = s.x_m < 2.0;
    const bool t_left = t.x_m < 2.0;
    if (s_left == t_left) {
        return distance(s, t);
    }
    const double y_at_wall = s.y_m + (t.y_m - s.y_m) * (2.0 - s.x_m) / (t.x_m - s.x_m);
    if (y_at_wall > 2.625 + 0.03) {
        return distance(s, t);
    }
    const localization_point3d_t left = { 1.975, 2.625, 0.0 };
    const localization_point3d_t right = { 2.025, 2.625, 0.0 };
    return s_left
        ? distance(s, left) + 0.05 + distance(right, t)
        : distance(s, right) + 0.05 + distance(left, t);
}


static void test_known_void_beats_straight_rays(void)
{
    const localization_point3d_t sensors[6] = {
        { 0.2, 0.2, 0.0 }, { 0.3, 3.8, 0.0 }, { 3.8, 0.2, 0.0 },
        { 3.7, 3.8, 0.0 }, { 0.4, 1.6, 0.0 }, { 3.6, 2.0, 0.0 },
    };
    const localization_point3d_t source = { 2.9, 1.2, 0.0 };
    double paths[6];
    for (size_t i = 0U; i < 6U; ++i) paths[i] = wall_path(sensors[i], source);
    localization_suea_pick_t picks[6];
    make_picks(sensors, paths, 6U, 0.0, 300.0, picks);
    localization_suea_config_t config = default_config();

    /* Straight rays: the scan did not show the wall. */
    localization_suea_grid_t blind;
    make_grid(&blind, (localization_point3d_t) { 0.0, 0.0, 0.0 }, (localization_point3d_t) { 4.0, 4.0, 0.0 }, 0.05);
    localization_suea_result_t straight;
    assert(localization_suea_locate(&blind, picks, 6U, &config, &straight) == ESP_OK);
    assert(straight.status == LOCALIZATION_SUEA_SOLVED);

    /* SUEA: the wall is an empty area in the grid. */
    localization_suea_grid_t aware;
    make_grid(&aware, (localization_point3d_t) { 0.0, 0.0, 0.0 }, (localization_point3d_t) { 4.0, 4.0, 0.0 }, 0.05);
    carve_empty(&aware, (localization_point3d_t) { 1.99, -1.0, -1.0 }, (localization_point3d_t) { 2.01, 2.601, 1.0 });
    localization_suea_result_t suea;
    assert(localization_suea_locate(&aware, picks, 6U, &config, &suea) == ESP_OK);
    assert(suea.status == LOCALIZATION_SUEA_SOLVED);
    assert(suea.empty_cell_count > 0U);

    const double straight_error = distance(straight.best_position, source);
    const double suea_error = distance(suea.best_position, source);
    assert(suea_error < 0.1);
    assert(straight_error > 3.0 * suea_error && straight_error > 0.2);
    assert(suea.max_detour_m > 0.5);
    assert(suea.best_residual_rms_us < straight.best_residual_rms_us);
    printf("[SUEA] known void: SUEA error=%.3f m (rms %.1f us, v=%.1f) vs straight-ray error=%.3f m (rms %.1f us)\n",
           suea_error, suea.best_residual_rms_us, suea.velocity_mps, straight_error, straight.best_residual_rms_us);
    localization_suea_grid_free(&blind);
    localization_suea_grid_free(&aware);
}


static void test_active_shots_identify_an_unknown_void(void)
{
    const localization_point3d_t lo = { 0.0, 0.0, 0.0 };
    const localization_point3d_t hi = { 4.0, 4.0, 0.0 };
    const localization_point3d_t void_lo = { 1.75, 0.8, -1.0 };
    const localization_point3d_t void_hi = { 2.25, 3.2, 1.0 };

    /* Truth: an interior cavity nobody could see from outside. */
    localization_suea_grid_t truth;
    make_grid(&truth, lo, hi, 0.1);
    carve_empty(&truth, void_lo, void_hi);

    const localization_point3d_t sensors[6] = {
        { 0.2, 0.3, 0.0 }, { 0.2, 3.7, 0.0 }, { 3.8, 0.3, 0.0 },
        { 3.8, 3.7, 0.0 }, { 0.3, 2.0, 0.0 }, { 3.7, 2.0, 0.0 },
    };
    const localization_point3d_t taps[10] = {
        { 0.6, 0.6, 0.0 }, { 0.6, 1.5, 0.0 }, { 0.6, 2.5, 0.0 }, { 0.6, 3.4, 0.0 },
        { 3.4, 0.6, 0.0 }, { 3.4, 1.5, 0.0 }, { 3.4, 2.5, 0.0 }, { 3.4, 3.4, 0.0 },
        { 2.0, 0.3, 0.0 }, { 2.0, 3.7, 0.0 },
    };
    const double v = 280.0;
    localization_suea_active_shot_t shots[10];
    memset(shots, 0, sizeof(shots));
    localization_suea_field_t truth_fields[6];
    for (size_t i = 0U; i < 6U; ++i) {
        assert(localization_suea_field_compute(&truth, &sensors[i], &truth_fields[i]) == ESP_OK);
    }
    for (size_t a = 0U; a < 10U; ++a) {
        shots[a].position = taps[a];
        shots[a].origin_time_known = true;
        shots[a].origin_time_us = 1000000 * (int64_t)(a + 1U);
        for (size_t i = 0U; i < 6U; ++i) {
            const double length = localization_suea_field_length_at(&truth, &truth_fields[i], &taps[a]);
            assert(isfinite(length));
            shots[a].arrival_time_us[i] = shots[a].origin_time_us + (int64_t)llround(length / v * 1.0e6);
            shots[a].arrival_valid[i] = true;
        }
    }

    /* The model starts all-UNKNOWN (passable). */
    localization_suea_grid_t model;
    make_grid(&model, lo, hi, 0.1);
    const localization_suea_identify_config_t identify = {
        .timing_uncertainty_us = 20.0,
        .late_threshold_sigma = 3.0,
        .on_time_threshold_sigma = 2.0,
        .max_iterations = 30U,
    };
    localization_suea_identify_result_t found;
    assert(localization_suea_identify_empty_areas(&model, sensors, 6U, shots, 10U, &identify, &found) == ESP_OK);
    assert(found.rays_used == 60U);
    assert(found.cells_marked_empty > 0U);
    assert(found.cells_certified_solid > 0U);
    assert(fabs(found.velocity_mps - v) < 0.05 * v);

    /* Binary ray tomography is non-unique -- a sparse wall of blocking
     * cells delays rays like a solid block -- so the check is what matters
     * for location: the identified cells explain the delays and sit ON the
     * real void, not scattered along the rays. */
    size_t marked_total = 0U;
    size_t marked_near = 0U;
    localization_point3d_t marked_centroid = { 0.0, 0.0, 0.0 };
    for (size_t iy = 0U; iy < model.ny; ++iy) {
        for (size_t ix = 0U; ix < model.nx; ++ix) {
            if (localization_suea_grid_get(&model, ix, iy, 0U) != LOCALIZATION_SUEA_CELL_EMPTY) {
                continue;
            }
            const localization_point3d_t c = localization_suea_grid_center(&model, ix, iy, 0U);
            const double dx = c.x_m < void_lo.x_m ? void_lo.x_m - c.x_m : (c.x_m > void_hi.x_m ? c.x_m - void_hi.x_m : 0.0);
            const double dy = c.y_m < void_lo.y_m ? void_lo.y_m - c.y_m : (c.y_m > void_hi.y_m ? c.y_m - void_hi.y_m : 0.0);
            ++marked_total;
            marked_near += sqrt(dx * dx + dy * dy) <= 0.35 ? 1U : 0U;
            marked_centroid.x_m += c.x_m;
            marked_centroid.y_m += c.y_m;
        }
    }
    assert(marked_total == found.cells_marked_empty);
    assert(marked_total > 0U);
    marked_centroid.x_m /= (double)marked_total;
    marked_centroid.y_m /= (double)marked_total;
    assert(found.rms_after_us < 0.1 * found.rms_before_us);
    assert((double)marked_near >= 0.8 * (double)marked_total);
    assert(fabs(marked_centroid.x_m - 2.0) < 0.2 && fabs(marked_centroid.y_m - 2.0) < 0.3);
    printf("[SUEA] identification: rms %.1f -> %.1f us, %zu cells kept (%zu pruned), %zu within 0.35 m of the void, centroid (%.2f, %.2f), %zu certified solid, v=%.1f\n",
           found.rms_before_us, found.rms_after_us, marked_total, found.cells_pruned, marked_near,
           marked_centroid.x_m, marked_centroid.y_m, found.cells_certified_solid, found.velocity_mps);

    /* An unknown source behind the void: locate with the identified model
     * versus the unidentified (all-passable) one. */
    const localization_point3d_t source = { 2.9, 2.1, 0.0 };
    localization_suea_pick_t picks[6];
    for (size_t i = 0U; i < 6U; ++i) {
        picks[i].sensor_position = sensors[i];
        picks[i].arrival_time_us =
            (int64_t)llround(localization_suea_field_length_at(&truth, &truth_fields[i], &source) / v * 1.0e6);
        picks[i].weight = 1.0;
    }
    localization_suea_grid_t unidentified;
    make_grid(&unidentified, lo, hi, 0.1);
    localization_suea_config_t config = default_config();
    localization_suea_result_t before;
    localization_suea_result_t after;
    assert(localization_suea_locate(&unidentified, picks, 6U, &config, &before) == ESP_OK);
    assert(localization_suea_locate(&model, picks, 6U, &config, &after) == ESP_OK);
    assert(before.status == LOCALIZATION_SUEA_SOLVED && after.status == LOCALIZATION_SUEA_SOLVED);
    const double before_error = distance(before.best_position, source);
    const double after_error = distance(after.best_position, source);
    assert(after_error < before_error);
    assert(after_error < 0.25);
    printf("[SUEA] source behind the identified void: error %.3f m -> %.3f m\n", before_error, after_error);

    for (size_t i = 0U; i < 6U; ++i) {
        localization_suea_field_free(&truth_fields[i]);
    }
    localization_suea_grid_free(&truth);
    localization_suea_grid_free(&model);
    localization_suea_grid_free(&unidentified);
}


static void test_identification_never_overrules_scan_evidence(void)
{
    const localization_point3d_t lo = { 0.0, 0.0, 0.0 };
    const localization_point3d_t hi = { 2.0, 2.0, 0.0 };
    localization_suea_grid_t grid;
    make_grid(&grid, lo, hi, 0.1);
    /* Everything is scan-certified SOLID: nothing may become EMPTY even if
     * the (deliberately inconsistent) arrivals are late. */
    for (size_t iy = 0U; iy < grid.ny; ++iy) {
        for (size_t ix = 0U; ix < grid.nx; ++ix) {
            localization_suea_grid_set(&grid, ix, iy, 0U, LOCALIZATION_SUEA_CELL_SOLID);
        }
    }
    const localization_point3d_t sensors[3] = { { 0.1, 0.1, 0.0 }, { 1.9, 0.1, 0.0 }, { 1.0, 1.9, 0.0 } };
    localization_suea_active_shot_t shot;
    memset(&shot, 0, sizeof(shot));
    shot.position = (localization_point3d_t) { 1.0, 0.8, 0.0 };
    shot.origin_time_known = true;
    for (size_t i = 0U; i < 3U; ++i) {
        shot.arrival_time_us[i] = (int64_t)llround(distance(sensors[i], shot.position) / 300.0 * 1.0e6) +
            (i == 2U ? 3000 : 0);
        shot.arrival_valid[i] = true;
    }
    const localization_suea_identify_config_t identify = {
        .timing_uncertainty_us = 20.0, .late_threshold_sigma = 3.0, .on_time_threshold_sigma = 2.0, .max_iterations = 5U,
    };
    localization_suea_identify_result_t found;
    assert(localization_suea_identify_empty_areas(&grid, sensors, 3U, &shot, 1U, &identify, &found) == ESP_OK);
    assert(found.cells_marked_empty == 0U);
    assert(localization_suea_grid_count(&grid, LOCALIZATION_SUEA_CELL_EMPTY) == 0U);

    {
        localization_suea_identify_config_t bad = identify;
        bad.late_threshold_sigma = 1.0; /* not above the on-time band */
        assert(localization_suea_identify_empty_areas(&grid, sensors, 3U, &shot, 1U, &bad, &found) == ESP_ERR_INVALID_ARG);
        assert(localization_suea_identify_empty_areas(&grid, sensors, 0U, &shot, 1U, &identify, &found) == ESP_ERR_INVALID_ARG);
        assert(localization_suea_identify_empty_areas(&grid, sensors, 3U, &shot, 0U, &identify, &found) == ESP_ERR_INVALID_ARG);
    }
    localization_suea_grid_free(&grid);
}


/* --- 3D: a scanned trench and open air above the surface ------------------ */

typedef struct
{
    double trench_low_x;
    double trench_high_x;
    double trench_bottom_z;

} scene_t;


/* What the phone scan reports: air above the surface and inside the trench
 * (both seen by the camera), a thin observed surface skin, interior unknown. */
static localization_suea_cell_t scan_classify(const localization_point3d_t *p, void *context)
{
    const scene_t *scene = context;
    if (p->z_m > 0.0) {
        return LOCALIZATION_SUEA_CELL_EMPTY;
    }
    if (p->x_m > scene->trench_low_x && p->x_m < scene->trench_high_x && p->z_m > scene->trench_bottom_z) {
        return LOCALIZATION_SUEA_CELL_EMPTY;
    }
    if (p->z_m > -0.1) {
        return LOCALIZATION_SUEA_CELL_SOLID;
    }
    return LOCALIZATION_SUEA_CELL_UNKNOWN;
}


/* The trench is invariant along y: the shortest path's x-z projection is
 * the 2D path under the trench bottom corners, y varies linearly. */
static double trench_path(const scene_t *scene, localization_point3d_t s, localization_point3d_t t)
{
    const bool s_left = s.x_m < scene->trench_low_x;
    const bool t_left = t.x_m < scene->trench_low_x;
    const double dy = t.y_m - s.y_m;
    if (s_left == t_left) {
        return distance(s, t);
    }
    const localization_point3d_t a = s_left ? s : t;
    const localization_point3d_t b = s_left ? t : s;
    const double xa = a.x_m, za = a.z_m, xb = b.x_m, zb = b.z_m;
    /* Straight x-z segment below both bottom corners? */
    const double z_at_low = za + (zb - za) * (scene->trench_low_x - xa) / (xb - xa);
    const double z_at_high = za + (zb - za) * (scene->trench_high_x - xa) / (xb - xa);
    double xz = 0.0;
    if (z_at_low <= scene->trench_bottom_z && z_at_high <= scene->trench_bottom_z) {
        xz = sqrt((xb - xa) * (xb - xa) + (zb - za) * (zb - za));
    } else {
        const double d1x = scene->trench_low_x - xa, d1z = scene->trench_bottom_z - za;
        const double d2x = xb - scene->trench_high_x, d2z = zb - scene->trench_bottom_z;
        const double via_low = sqrt(d1x * d1x + d1z * d1z);
        const double via_high = sqrt(d2x * d2x + d2z * d2z);
        /* The path hugs the bottom only if the far point is not visible
         * from the near corner; otherwise it bends at one corner. */
        const double bottom = scene->trench_high_x - scene->trench_low_x;
        const double direct_from_low = sqrt((xb - scene->trench_low_x) * (xb - scene->trench_low_x) + d2z * d2z);
        const double z_line = scene->trench_bottom_z + (zb - scene->trench_bottom_z) *
            (scene->trench_high_x - scene->trench_low_x) / (xb - scene->trench_low_x);
        xz = z_line <= scene->trench_bottom_z + 1.0e-12
            ? via_low + direct_from_low
            : via_low + bottom + via_high;
    }
    return sqrt(xz * xz + dy * dy);
}


static void test_scanned_trench_3d(void)
{
    const scene_t scene = { .trench_low_x = 1.35, .trench_high_x = 1.65, .trench_bottom_z = -0.85 };
    const localization_point3d_t sensors[6] = {
        { 0.3, 0.4, 0.0 }, { 0.4, 2.6, 0.0 }, { 0.9, 1.5, 0.0 },
        { 2.6, 0.3, 0.0 }, { 2.7, 2.7, 0.0 }, { 2.2, 1.4, 0.0 },
    };
    const localization_point3d_t source = { 2.2, 1.9, -1.2 };
    double paths[6];
    for (size_t i = 0U; i < 6U; ++i) paths[i] = trench_path(&scene, sensors[i], source);
    localization_suea_pick_t picks[6];
    make_picks(sensors, paths, 6U, 0.25, 240.0, picks);

    const localization_point3d_t lo = { 0.0, 0.0, -2.0 };
    const localization_point3d_t hi = { 3.0, 3.0, 0.3 };

    localization_suea_grid_t scanned;
    make_grid(&scanned, lo, hi, 0.1);
    localization_suea_grid_classify(&scanned, scan_classify, (void *)&scene);
    assert(localization_suea_grid_count(&scanned, LOCALIZATION_SUEA_CELL_EMPTY) > 0U);
    assert(localization_suea_grid_count(&scanned, LOCALIZATION_SUEA_CELL_SOLID) > 0U);
    assert(localization_suea_grid_count(&scanned, LOCALIZATION_SUEA_CELL_UNKNOWN) > 0U);

    localization_suea_grid_t blind;
    make_grid(&blind, lo, hi, 0.1);

    localization_suea_config_t config = default_config();
    localization_suea_result_t aware;
    localization_suea_result_t straight;
    assert(localization_suea_locate(&scanned, picks, 6U, &config, &aware) == ESP_OK);
    assert(localization_suea_locate(&blind, picks, 6U, &config, &straight) == ESP_OK);
    assert(aware.status == LOCALIZATION_SUEA_SOLVED && straight.status == LOCALIZATION_SUEA_SOLVED);
    const double aware_error = distance(aware.best_position, source);
    const double straight_error = distance(straight.best_position, source);
    /* Fully velocity-free over a flat surface array, depth trades against
     * velocity; the any-angle paths pivot on cell centers, half a cell from
     * the true trench corner. Two cells is the honest bound here. */
    assert(aware_error < 2.0 * scanned.cell_m + 0.05);
    assert(straight_error > 2.5 * aware_error);
    assert(aware.best_position.z_m < 0.0);
    assert(aware.max_detour_m > 0.05);
    printf("[SUEA] 3D scanned trench, velocity-free: scan-aware error=%.3f m vs straight-ray error=%.3f m, detour=%.3f m, v=%.1f\n",
           aware_error, straight_error, aware.max_detour_m, aware.velocity_mps);

    /* A tap-calibrated velocity (MAX-139) enters as a tight prior and
     * removes the depth/velocity trade-off of the flat array. */
    config.prior_velocity_mps = 240.0;
    config.prior_velocity_sigma_mps = 1.0;
    localization_suea_result_t with_prior;
    assert(localization_suea_locate(&scanned, picks, 6U, &config, &with_prior) == ESP_OK);
    assert(with_prior.status == LOCALIZATION_SUEA_SOLVED);
    const double prior_error = distance(with_prior.best_position, source);
    assert(fabs(with_prior.velocity_mps - 240.0) < 3.0);
    assert(prior_error < aware_error);
    printf("[SUEA] 3D scanned trench, calibrated velocity 240+/-1 m/s: error=%.3f m, v=%.1f\n",
           prior_error, with_prior.velocity_mps);

    /* A fixed prior velocity (sigma 0) also works, e.g. with fewer sensors. */
    config.prior_velocity_mps = 240.0;
    config.prior_velocity_sigma_mps = 0.0;
    localization_suea_result_t fixed;
    assert(localization_suea_locate(&scanned, picks, 4U, &config, &fixed) == ESP_OK);
    assert(fixed.status == LOCALIZATION_SUEA_SOLVED);
    assert(fabs(fixed.velocity_mps - 240.0) < 1.0e-9);
    localization_suea_grid_free(&scanned);
    localization_suea_grid_free(&blind);
}


static bool reject_everything(const localization_point3d_t *candidate, void *context)
{
    (void)candidate;
    (void)context;
    return false;
}


static void test_sensor_snapping_filters_and_invalid_input(void)
{
    const localization_point3d_t lo = { 0.0, 0.0, -1.0 };
    const localization_point3d_t hi = { 2.0, 2.0, 0.5 };
    localization_suea_grid_t grid;
    make_grid(&grid, lo, hi, 0.1);
    /* Open air above z = 0. */
    carve_empty(&grid, (localization_point3d_t) { -1.0, -1.0, 0.0 }, (localization_point3d_t) { 3.0, 3.0, 1.0 });

    /* Registered 4 cm into the air above the surface: snapped back. */
    const localization_point3d_t hovering = { 1.0, 1.0, 0.09 };
    localization_suea_field_t field;
    assert(localization_suea_field_compute(&grid, &hovering, &field) == ESP_OK);
    localization_suea_field_free(&field);
    /* Deep in open air: no passable cell within two cells. */
    const localization_point3d_t floating = { 1.0, 1.0, 0.45 };
    assert(localization_suea_field_compute(&grid, &floating, &field) == ESP_ERR_NOT_FOUND);
    const localization_point3d_t outside = { 9.0, 1.0, 0.0 };
    assert(localization_suea_field_compute(&grid, &outside, &field) == ESP_ERR_NOT_FOUND);

    localization_suea_pick_t picks[4] = {
        { { 0.1, 0.1, -0.05 }, 1000, 1.0 },
        { { 1.9, 0.1, -0.05 }, 1500, 1.0 },
        { { 1.0, 1.9, 0.45 }, 1200, 1.0 },  /* unreachable */
        { { 1.0, 1.0, -0.05 }, 1100, 0.0 }, /* excluded by weight */
    };
    localization_suea_config_t config = default_config();
    localization_suea_result_t result;
    assert(localization_suea_locate(&grid, picks, 4U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_SUEA_INSUFFICIENT_PICKS);
    assert(result.pick_mask_unreachable == (1U << 2));
    assert(result.pick_mask_used == 0x3U);

    picks[2].sensor_position.z_m = -0.05;
    picks[3].weight = 1.0;
    config.candidate_filter.allowed = reject_everything;
    assert(localization_suea_locate(&grid, picks, 4U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_SUEA_NO_ALLOWED_CANDIDATE);

    config = default_config();
    config.refine_divisor = 1U;
    assert(localization_suea_locate(&grid, picks, 4U, &config, &result) == ESP_ERR_INVALID_ARG);
    config = default_config();
    config.timing_uncertainty_us = 0.0;
    assert(localization_suea_locate(&grid, picks, 4U, &config, &result) == ESP_ERR_INVALID_ARG);
    config = default_config();
    assert(localization_suea_locate(&grid, picks, 0U, &config, &result) == ESP_ERR_INVALID_ARG);
    assert(localization_suea_locate(NULL, picks, 4U, &config, &result) == ESP_ERR_INVALID_ARG);
    localization_suea_grid_free(&grid);
}


int main(void)
{
    test_grid_geometry_and_budget();
    test_unobstructed_field_is_exact_straight_distance();
    test_path_detours_around_a_wall();
    test_velocity_free_location_in_homogeneous_medium();
    test_known_void_beats_straight_rays();
    test_active_shots_identify_an_unknown_void();
    test_identification_never_overrules_scan_evidence();
    test_scanned_trench_3d();
    test_sensor_snapping_filters_and_invalid_input();
    printf("localization SUEA tests passed\n");
    return 0;
}
