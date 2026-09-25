#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"

/*
 * SUEA -- AE/MS source location for Structures containing Unknown Empty
 * Areas (L. Dong, Q. Tao, Q. Hu et al., International Journal of Mining
 * Science and Technology 32(3) (2022) 487-497).
 *
 * A wave cannot cross an empty area (a void, a gap in rubble, open air):
 * it travels the shortest path AROUND it, so straight-ray travel times are
 * wrong for every sensor whose direct ray crosses one. SUEA
 *
 *   1. discretizes the structure into a voxel grid whose cells are
 *      SOLID / UNKNOWN / EMPTY;
 *   2. identifies the unknown empty areas from ACTIVE sources (shots at
 *      known positions, e.g. hammer taps) -- their arrivals come later than
 *      the current model predicts exactly where an unmodelled void lies on
 *      the ray (localization_suea_identify_empty_areas());
 *   3. computes, for every sensor, the shortest non-empty travel path to
 *      every grid point (an A*-family search; here Lazy Theta*, an
 *      any-angle A* variant whose paths are not restricted to the 26 grid
 *      directions, run once per sensor for the whole field). A ray may
 *      touch an empty cell's edge or corner, but never squeeze between two
 *      empty cells that share one. The search runs in single precision
 *      (the ESP32-S3 FPU); lengths are reported in meters;
 *   4. locates the source without a pre-measured velocity: at each grid
 *      point the arrivals are fitted as t_i = t0 + L_i / v, and the point
 *      whose fit deviates least wins (localization_suea_locate()).
 *
 * In this project the cells come first from the phone 3D scan
 * (components/scan_occupancy): scan-verified FREE air is EMPTY, observed
 * surface is SOLID, and the unseen interior stays UNKNOWN -- passable,
 * never assumed empty (the project-wide invariant). Active shots can then
 * turn UNKNOWN interior cells EMPTY (a void found by its delay) or SOLID
 * (crossed by an on-time ray).
 *
 * A caller-supplied velocity may optionally enter as a Gaussian prior on
 * the slowness (prior_velocity_sigma_mps); with prior_velocity_mps <= 0
 * the search is fully velocity-free, as in the paper.
 */

#define LOCALIZATION_SUEA_MAX_SENSORS LOCALIZATION_MAX_SENSORS
#define LOCALIZATION_SUEA_MAX_SHOTS 16U

/* 40000 cells x (4 B length + 4 B parent) x 8 sensors ~= 2.5 MiB worst case
 * (4 sensors on the current hardware: ~1.3 MiB) plus ~0.7 MiB of search
 * working set: PSRAM-sized, bounded. */
#define LOCALIZATION_SUEA_DEFAULT_MAX_CELLS 40000U


typedef enum
{
    /* Unseen: passable, never treated as empty without evidence. */
    LOCALIZATION_SUEA_CELL_UNKNOWN = 0,

    /* Certified material (observed surface, or crossed by an on-time ray). */
    LOCALIZATION_SUEA_CELL_SOLID,

    /* Blocks propagation: verified free air or an identified empty area. */
    LOCALIZATION_SUEA_CELL_EMPTY

} localization_suea_cell_t;


typedef struct
{
    localization_point3d_t origin; /* min corner of cell (0, 0, 0) */
    double cell_m;
    size_t nx;
    size_t ny;
    size_t nz;
    uint8_t *cells; /* nx * ny * nz, x fastest; owned */

} localization_suea_grid_t;


/*
 * Allocates a grid covering [min_corner, max_corner] with cells of
 * requested_cell_m, enlarged by the smallest integer factor that keeps the
 * cell count within max_cells (0 selects the default). Every cell starts
 * UNKNOWN. A flat box (min.z == max.z) gives a single-layer, planar grid.
 */
esp_err_t localization_suea_grid_init(
    localization_suea_grid_t *grid,
    const localization_point3d_t *min_corner,
    const localization_point3d_t *max_corner,
    double requested_cell_m,
    size_t max_cells
);

void localization_suea_grid_free(localization_suea_grid_t *grid);

size_t localization_suea_grid_cell_count(const localization_suea_grid_t *grid);

/* False when the point lies outside the grid. */
bool localization_suea_grid_locate(
    const localization_suea_grid_t *grid,
    const localization_point3d_t *point,
    size_t *ix,
    size_t *iy,
    size_t *iz
);

localization_point3d_t localization_suea_grid_center(
    const localization_suea_grid_t *grid,
    size_t ix,
    size_t iy,
    size_t iz
);

localization_suea_cell_t localization_suea_grid_get(
    const localization_suea_grid_t *grid,
    size_t ix,
    size_t iy,
    size_t iz
);

void localization_suea_grid_set(
    localization_suea_grid_t *grid,
    size_t ix,
    size_t iy,
    size_t iz,
    localization_suea_cell_t cell
);

size_t localization_suea_grid_count(
    const localization_suea_grid_t *grid,
    localization_suea_cell_t cell
);


typedef localization_suea_cell_t (*localization_suea_classify_fn)(
    const localization_point3d_t *point,
    void *context
);

/*
 * Classifies every cell from 9 samples (its center and the centers of its
 * eight octants). EMPTY needs a strict majority of EMPTY samples -- a cell
 * only blocks waves when it is mostly empty; otherwise SOLID if any sample
 * is SOLID; otherwise UNKNOWN.
 */
void localization_suea_grid_classify(
    localization_suea_grid_t *grid,
    localization_suea_classify_fn classify,
    void *context
);


/* ---- Shortest travel paths ------------------------------------------- */

typedef struct
{
    localization_point3d_t source;
    size_t count;
    float *length_m; /* per cell center; INFINITY where unreachable/EMPTY */
    int32_t *parent; /* any-angle predecessor cell; -1 = the source point */

} localization_suea_field_t;

/*
 * Shortest non-empty path length from `source` to every cell center. A
 * source inside an EMPTY cell (e.g. a sensor registered a few cm into
 * the air above the surface) starts from the nearest passable cell within
 * two cells; ESP_ERR_NOT_FOUND if there is none or the source lies outside
 * the grid. ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t localization_suea_field_compute(
    const localization_suea_grid_t *grid,
    const localization_point3d_t *source,
    localization_suea_field_t *field
);

void localization_suea_field_free(localization_suea_field_t *field);

/* Path length to an arbitrary point: the any-angle predecessor's length
 * plus the straight remainder when visible (exact for an unobstructed
 * ray). INFINITY when the point is unreachable. */
double localization_suea_field_length_at(
    const localization_suea_grid_t *grid,
    const localization_suea_field_t *field,
    const localization_point3d_t *point
);


/* ---- Velocity-free location ------------------------------------------- */

typedef struct
{
    localization_point3d_t sensor_position;
    int64_t arrival_time_us;
    double weight; /* <= 0 excludes the pick; 1.0 is a normal pick */

} localization_suea_pick_t;


typedef struct
{
    /* Assumed 1-sigma pick noise (> 0): scales the fit objective. */
    double timing_uncertainty_us;

    /* <= 0: velocity-free. Otherwise a Gaussian prior on the slowness with
     * this sigma in m/s; a sigma of exactly 0 fixes the velocity. */
    double prior_velocity_mps;
    double prior_velocity_sigma_mps;

    /* Sub-cell refinement around the best cell at cell_m / divisor (>= 2). */
    uint32_t refine_divisor;

    localization_candidate_filter_t candidate_filter;

} localization_suea_config_t;


typedef enum
{
    LOCALIZATION_SUEA_SOLVED = 0,

    /* Fewer than three usable picks, possibly after excluding sensors that
     * sit inside empty space (see pick_mask_unreachable). */
    LOCALIZATION_SUEA_INSUFFICIENT_PICKS,

    /* No grid point is reachable from every sensor with a physical fit. */
    LOCALIZATION_SUEA_NO_REACHABLE_CANDIDATE,

    /* The candidate filter rejected every reachable grid point. */
    LOCALIZATION_SUEA_NO_ALLOWED_CANDIDATE

} localization_suea_status_t;


typedef struct
{
    localization_suea_status_t status;
    size_t picks_used_count;
    uint32_t pick_mask_used;
    uint32_t pick_mask_unreachable;

    localization_point3d_t best_position;
    double best_residual_rms_us;
    double velocity_mps;
    int64_t emission_time_us;

    /* Largest (path length - straight distance) over the used sensors at
     * the best position: how far the empty areas bent the rays. */
    double max_detour_m;

    size_t empty_cell_count;
    double cell_m_used;
    size_t candidates_evaluated;

} localization_suea_result_t;


esp_err_t localization_suea_locate(
    const localization_suea_grid_t *grid,
    const localization_suea_pick_t *picks,
    size_t pick_count,
    const localization_suea_config_t *config,
    localization_suea_result_t *result
);


/* ---- Empty-area identification from active sources --------------------- */

typedef struct
{
    localization_point3d_t position;

    /* A triggered source (e.g. an instrumented hammer) knows its origin
     * time; otherwise it is fitted per shot. */
    bool origin_time_known;
    int64_t origin_time_us;

    int64_t arrival_time_us[LOCALIZATION_SUEA_MAX_SENSORS];
    bool arrival_valid[LOCALIZATION_SUEA_MAX_SENSORS];

} localization_suea_active_shot_t;


typedef struct
{
    double timing_uncertainty_us;   /* > 0 */
    double late_threshold_sigma;    /* residual above this x sigma: delayed ray */
    double on_time_threshold_sigma; /* |residual| within this x sigma: ray path certified SOLID */
    uint32_t max_iterations;

} localization_suea_identify_config_t;


typedef struct
{
    size_t iterations;
    size_t rays_used;
    size_t cells_marked_empty;    /* EMPTY cells kept after elimination */
    size_t cells_pruned;          /* marked, then removed as unnecessary */
    size_t cells_certified_solid;
    double rms_before_us;
    double rms_after_us;
    double velocity_mps;

} localization_suea_identify_result_t;


/*
 * Turns UNKNOWN cells into EMPTY (a void delaying the rays that cross it)
 * or SOLID (crossed by an on-time ray) from active shots. Never changes a
 * cell that is already SOLID or EMPTY -- scan evidence is not overruled.
 * Each round marks the unknown cells most consistently crossed by delayed
 * rays of at least two sensors and two shots (and by no on-time ray) and
 * keeps them only if the refitted travel-time misfit drops -- the most
 * consistently delayed cells first, a looser selection only if those did
 * not help; when none helps, the growth stops. A final backward
 * elimination removes edge cells the misfit does not need.
 */
esp_err_t localization_suea_identify_empty_areas(
    localization_suea_grid_t *grid,
    const localization_point3d_t *sensor_positions,
    size_t sensor_count,
    const localization_suea_active_shot_t *shots,
    size_t shot_count,
    const localization_suea_identify_config_t *config,
    localization_suea_identify_result_t *result
);
