#include "localization_suea.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "localization_vf_math.h"


#define SUEA_MIN_PICKS 3U
#define SUEA_SNAP_RADIUS_CELLS 2
#define SUEA_NO_PARENT (-2)
#define SUEA_SOURCE_PARENT (-1)
#define SUEA_MAX_CELL_FACTOR 100000U

/* An identification round is kept only if it lowers the travel-time
 * misfit (sum of squares) by at least this fraction. */
#define SUEA_MIN_IDENTIFY_IMPROVEMENT 1.0e-3

/* A round marks the cells crossed by delayed rays at least this fraction of
 * the strongest one -- the strictest first; a looser fraction is tried only
 * when the stricter marks did not lower the misfit. */
static const float SUEA_IDENTIFY_SCORE_FRACTIONS[] = { 0.8f, 0.65f, 0.5f };

/* Upper bound on single-cell elimination tests per pass (each re-runs the
 * path search for every sensor). */
#define SUEA_MAX_PRUNE_TESTS 512U

enum
{
    SUEA_STATE_NEW = 0,
    SUEA_STATE_OPEN,
    SUEA_STATE_CLOSED
};


/* ---- Grid ------------------------------------------------------------- */

static size_t cell_index(const localization_suea_grid_t *grid, size_t ix, size_t iy, size_t iz)
{
    return (iz * grid->ny + iy) * grid->nx + ix;
}


static void cell_coords(const localization_suea_grid_t *grid, size_t index, size_t *ix, size_t *iy, size_t *iz)
{
    *ix = index % grid->nx;
    *iy = (index / grid->nx) % grid->ny;
    *iz = index / (grid->nx * grid->ny);
}


static localization_point3d_t index_center(const localization_suea_grid_t *grid, size_t index)
{
    size_t ix = 0U, iy = 0U, iz = 0U;
    cell_coords(grid, index, &ix, &iy, &iz);
    return localization_suea_grid_center(grid, ix, iy, iz);
}


static size_t axis_cells(double extent, double cell, size_t limit)
{
    const double intervals = floor(extent / cell);
    if (!isfinite(intervals) || intervals < 0.0 || intervals >= (double)limit) {
        return 0U;
    }
    return (size_t)intervals + 1U;
}


esp_err_t localization_suea_grid_init(
    localization_suea_grid_t *grid,
    const localization_point3d_t *min_corner,
    const localization_point3d_t *max_corner,
    double requested_cell_m,
    size_t max_cells
)
{
    if (grid == NULL || min_corner == NULL || max_corner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(grid, 0, sizeof(*grid));

    const double extent[3] = {
        max_corner->x_m - min_corner->x_m,
        max_corner->y_m - min_corner->y_m,
        max_corner->z_m - min_corner->z_m,
    };
    if (
        !isfinite(min_corner->x_m) || !isfinite(min_corner->y_m) || !isfinite(min_corner->z_m) ||
        !isfinite(extent[0]) || !isfinite(extent[1]) || !isfinite(extent[2]) ||
        extent[0] < 0.0 || extent[1] < 0.0 || extent[2] < 0.0 ||
        !(requested_cell_m > 0.0) || !isfinite(requested_cell_m)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t limit = max_cells > 0U ? max_cells : LOCALIZATION_SUEA_DEFAULT_MAX_CELLS;
    double cell = requested_cell_m;
    size_t n[3] = { 0U, 0U, 0U };
    for (size_t factor = 1U; ; ++factor) {
        if (factor > SUEA_MAX_CELL_FACTOR) {
            return ESP_ERR_INVALID_SIZE;
        }
        cell = requested_cell_m * (double)factor;
        if (!isfinite(cell)) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t axis = 0U; axis < 3U; ++axis) {
            n[axis] = axis_cells(extent[axis], cell, limit);
        }
        if (n[0] != 0U && n[1] != 0U && n[2] != 0U &&
            n[0] <= limit / n[1] && n[0] * n[1] <= limit / n[2]) {
            break;
        }
    }

    grid->cells = calloc(n[0] * n[1] * n[2], 1U);
    if (grid->cells == NULL) {
        return ESP_ERR_NO_MEM;
    }
    grid->cell_m = cell;
    grid->nx = n[0];
    grid->ny = n[1];
    grid->nz = n[2];
    /* Centered on the box: a flat box puts its single layer's centers
     * exactly on min.z == max.z. */
    grid->origin = (localization_point3d_t) {
        min_corner->x_m - 0.5 * ((double)n[0] * cell - extent[0]),
        min_corner->y_m - 0.5 * ((double)n[1] * cell - extent[1]),
        min_corner->z_m - 0.5 * ((double)n[2] * cell - extent[2]),
    };
    return ESP_OK;
}


void localization_suea_grid_free(localization_suea_grid_t *grid)
{
    if (grid == NULL) {
        return;
    }
    free(grid->cells);
    memset(grid, 0, sizeof(*grid));
}


size_t localization_suea_grid_cell_count(const localization_suea_grid_t *grid)
{
    return grid == NULL ? 0U : grid->nx * grid->ny * grid->nz;
}


bool localization_suea_grid_locate(
    const localization_suea_grid_t *grid,
    const localization_point3d_t *point,
    size_t *ix,
    size_t *iy,
    size_t *iz
)
{
    if (grid == NULL || grid->cells == NULL || point == NULL) {
        return false;
    }
    const double f[3] = {
        (point->x_m - grid->origin.x_m) / grid->cell_m,
        (point->y_m - grid->origin.y_m) / grid->cell_m,
        (point->z_m - grid->origin.z_m) / grid->cell_m,
    };
    const size_t n[3] = { grid->nx, grid->ny, grid->nz };
    size_t index[3];
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (!isfinite(f[axis]) || f[axis] < 0.0 || f[axis] >= (double)n[axis]) {
            return false;
        }
        index[axis] = (size_t)floor(f[axis]);
    }
    *ix = index[0];
    *iy = index[1];
    *iz = index[2];
    return true;
}


localization_point3d_t localization_suea_grid_center(
    const localization_suea_grid_t *grid,
    size_t ix,
    size_t iy,
    size_t iz
)
{
    return (localization_point3d_t) {
        grid->origin.x_m + ((double)ix + 0.5) * grid->cell_m,
        grid->origin.y_m + ((double)iy + 0.5) * grid->cell_m,
        grid->origin.z_m + ((double)iz + 0.5) * grid->cell_m,
    };
}


localization_suea_cell_t localization_suea_grid_get(
    const localization_suea_grid_t *grid,
    size_t ix,
    size_t iy,
    size_t iz
)
{
    return (localization_suea_cell_t)grid->cells[cell_index(grid, ix, iy, iz)];
}


void localization_suea_grid_set(
    localization_suea_grid_t *grid,
    size_t ix,
    size_t iy,
    size_t iz,
    localization_suea_cell_t cell
)
{
    grid->cells[cell_index(grid, ix, iy, iz)] = (uint8_t)cell;
}


size_t localization_suea_grid_count(
    const localization_suea_grid_t *grid,
    localization_suea_cell_t cell
)
{
    size_t count = 0U;
    const size_t total = localization_suea_grid_cell_count(grid);
    for (size_t i = 0U; i < total; ++i) {
        if (grid->cells[i] == (uint8_t)cell) {
            ++count;
        }
    }
    return count;
}


void localization_suea_grid_classify(
    localization_suea_grid_t *grid,
    localization_suea_classify_fn classify,
    void *context
)
{
    if (grid == NULL || grid->cells == NULL || classify == NULL) {
        return;
    }
    static const double offsets[9][3] = {
        { 0.0, 0.0, 0.0 },
        { -0.25, -0.25, -0.25 }, { 0.25, -0.25, -0.25 }, { -0.25, 0.25, -0.25 }, { 0.25, 0.25, -0.25 },
        { -0.25, -0.25, 0.25 }, { 0.25, -0.25, 0.25 }, { -0.25, 0.25, 0.25 }, { 0.25, 0.25, 0.25 },
    };
    const size_t total = localization_suea_grid_cell_count(grid);
    for (size_t i = 0U; i < total; ++i) {
        const localization_point3d_t center = index_center(grid, i);
        size_t empty = 0U;
        size_t solid = 0U;
        for (size_t s = 0U; s < 9U; ++s) {
            const localization_point3d_t sample = {
                center.x_m + offsets[s][0] * grid->cell_m,
                center.y_m + offsets[s][1] * grid->cell_m,
                center.z_m + offsets[s][2] * grid->cell_m,
            };
            const localization_suea_cell_t cls = classify(&sample, context);
            if (cls == LOCALIZATION_SUEA_CELL_EMPTY) {
                ++empty;
            } else if (cls == LOCALIZATION_SUEA_CELL_SOLID) {
                ++solid;
            }
        }
        grid->cells[i] = (uint8_t)(
            2U * empty > 9U ? LOCALIZATION_SUEA_CELL_EMPTY :
            solid > 0U ? LOCALIZATION_SUEA_CELL_SOLID :
            LOCALIZATION_SUEA_CELL_UNKNOWN
        );
    }
}


/* ---- Voxel traversal (Amanatides-Woo) --------------------------------- */

static bool cell_empty_at(const localization_suea_grid_t *grid, const long cell[3])
{
    return grid->cells[cell_index(grid, (size_t)cell[0], (size_t)cell[1], (size_t)cell[2])] ==
        (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY;
}


/*
 * A segment crossing an edge or corner exactly (a tie between axes) passes
 * between the side cells, entering neither. It is sealed only when every
 * way round is EMPTY: for two tied axes, both side cells; for three, all
 * single-step or all double-step side cells. `tied` flags the axes that
 * step now; `cell` is the cell being left.
 */
static bool tie_sealed(const localization_suea_grid_t *grid, const long cell[3], const long step[3], const bool tied[3])
{
    size_t tied_count = 0U;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        tied_count += tied[axis] ? 1U : 0U;
    }
    if (tied_count < 2U) {
        return false;
    }
    bool single_open = false;
    bool double_open = false;
    for (size_t a = 0U; a < 3U; ++a) {
        if (!tied[a]) {
            continue;
        }
        long side[3] = { cell[0], cell[1], cell[2] };
        side[a] += step[a];
        single_open = single_open || !cell_empty_at(grid, side);
        for (size_t b = a + 1U; tied_count == 3U && b < 3U; ++b) {
            long corner[3] = { side[0], side[1], side[2] };
            corner[b] += step[b];
            double_open = double_open || !cell_empty_at(grid, corner);
        }
    }
    return tied_count == 2U ? !single_open : (!single_open || !double_open);
}

typedef bool (*suea_visit_fn)(size_t index, void *context);

/* Visits every cell the segment a -> b passes through, in order. False
 * when a visit stops it or the segment leaves the grid. */
static bool traverse(
    const localization_suea_grid_t *grid,
    localization_point3d_t a,
    localization_point3d_t b,
    suea_visit_fn visit,
    void *context
)
{
    const double ga[3] = {
        (a.x_m - grid->origin.x_m) / grid->cell_m,
        (a.y_m - grid->origin.y_m) / grid->cell_m,
        (a.z_m - grid->origin.z_m) / grid->cell_m,
    };
    const double gb[3] = {
        (b.x_m - grid->origin.x_m) / grid->cell_m,
        (b.y_m - grid->origin.y_m) / grid->cell_m,
        (b.z_m - grid->origin.z_m) / grid->cell_m,
    };
    const long n[3] = { (long)grid->nx, (long)grid->ny, (long)grid->nz };

    long cell[3];
    long end[3];
    long step[3];
    double t_max[3];
    double t_delta[3];
    long guard = 3;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        cell[axis] = (long)floor(ga[axis]);
        end[axis] = (long)floor(gb[axis]);
        /* A point exactly on the far boundary belongs to the last cell. */
        if (end[axis] == n[axis] && gb[axis] <= (double)n[axis]) {
            end[axis] = n[axis] - 1;
        }
        if (cell[axis] == n[axis] && ga[axis] <= (double)n[axis]) {
            cell[axis] = n[axis] - 1;
        }
        if (cell[axis] < 0 || cell[axis] >= n[axis] || end[axis] < 0 || end[axis] >= n[axis]) {
            return false;
        }
        const double d = gb[axis] - ga[axis];
        if (d > 0.0) {
            step[axis] = 1;
            t_max[axis] = ((double)cell[axis] + 1.0 - ga[axis]) / d;
            t_delta[axis] = 1.0 / d;
        } else if (d < 0.0) {
            step[axis] = -1;
            t_max[axis] = (ga[axis] - (double)cell[axis]) / -d;
            t_delta[axis] = -1.0 / d;
        } else {
            step[axis] = 0;
            t_max[axis] = INFINITY;
            t_delta[axis] = INFINITY;
        }
        guard += labs(end[axis] - cell[axis]);
    }

    if (!visit(cell_index(grid, (size_t)cell[0], (size_t)cell[1], (size_t)cell[2]), context)) {
        return false;
    }
    while ((cell[0] != end[0] || cell[1] != end[1] || cell[2] != end[2]) && guard-- > 0) {
        const double t_min = fmin(t_max[0], fmin(t_max[1], t_max[2]));
        if (!(t_min <= 1.0 + 1.0e-9)) {
            break;
        }
        /* Crossing an edge or corner exactly (a tie) steps diagonally:
         * touching a corner does not enter the side cells, unless they
         * seal the passage (tie_sealed()). */
        bool tied[3];
        for (size_t axis = 0U; axis < 3U; ++axis) {
            tied[axis] = t_max[axis] <= t_min + 1.0e-9;
            const long next = cell[axis] + (tied[axis] ? step[axis] : 0);
            if (next < 0 || next >= n[axis]) {
                return false;
            }
        }
        if (tie_sealed(grid, cell, step, tied)) {
            return false;
        }
        for (size_t axis = 0U; axis < 3U; ++axis) {
            if (tied[axis]) {
                cell[axis] += step[axis];
                t_max[axis] += t_delta[axis];
            }
        }
        if (!visit(cell_index(grid, (size_t)cell[0], (size_t)cell[1], (size_t)cell[2]), context)) {
            return false;
        }
    }
    return true;
}


static bool visit_not_empty(size_t index, void *context)
{
    const localization_suea_grid_t *grid = context;
    return grid->cells[index] != (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY;
}


static bool line_of_sight(const localization_suea_grid_t *grid, localization_point3d_t a, localization_point3d_t b)
{
    return traverse(grid, a, b, visit_not_empty, (void *)grid);
}


/* ---- Lazy Theta* over the whole field ---------------------------------
 *
 * The hot loop runs in single precision and in grid units (cell centers at
 * i + 0.5): the ESP32-S3 FPU is single-precision only, so double math here
 * would be software-emulated. Lengths are converted to meters once, at the
 * end; float resolves ~1e-5 cell over the grid sizes used. */

typedef struct
{
    const localization_suea_grid_t *grid;
    localization_suea_field_t *field;
    int32_t *heap;
    int32_t *heap_position;
    uint8_t *state;
    size_t heap_size;
    float source[3]; /* the source point in grid units */

} suea_search_t;


static void cell_center_grid(const localization_suea_grid_t *grid, size_t index, float out[3])
{
    size_t ix = 0U, iy = 0U, iz = 0U;
    cell_coords(grid, index, &ix, &iy, &iz);
    out[0] = (float)ix + 0.5f;
    out[1] = (float)iy + 0.5f;
    out[2] = (float)iz + 0.5f;
}


static void vertex_grid(const suea_search_t *search, int32_t vertex, float out[3])
{
    if (vertex < 0) {
        out[0] = search->source[0];
        out[1] = search->source[1];
        out[2] = search->source[2];
    } else {
        cell_center_grid(search->grid, (size_t)vertex, out);
    }
}


static float vertex_length(const suea_search_t *search, int32_t vertex)
{
    return vertex < 0 ? 0.0f : search->field->length_m[vertex];
}


static float distance_grid(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}


/* Single-precision line of sight in grid units (Amanatides-Woo): false when
 * the segment enters an EMPTY cell or leaves the grid. */
static bool line_of_sight_grid(const localization_suea_grid_t *grid, const float a[3], const float b[3])
{
    const long n[3] = { (long)grid->nx, (long)grid->ny, (long)grid->nz };
    long cell[3];
    long end[3];
    long step[3];
    float t_max[3];
    float t_delta[3];
    long guard = 3;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        cell[axis] = (long)floorf(a[axis]);
        end[axis] = (long)floorf(b[axis]);
        if (cell[axis] == n[axis]) cell[axis] = n[axis] - 1;
        if (end[axis] == n[axis]) end[axis] = n[axis] - 1;
        if (cell[axis] < 0 || cell[axis] >= n[axis] || end[axis] < 0 || end[axis] >= n[axis]) {
            return false;
        }
        const float d = b[axis] - a[axis];
        if (d > 0.0f) {
            step[axis] = 1;
            t_max[axis] = ((float)cell[axis] + 1.0f - a[axis]) / d;
            t_delta[axis] = 1.0f / d;
        } else if (d < 0.0f) {
            step[axis] = -1;
            t_max[axis] = (a[axis] - (float)cell[axis]) / -d;
            t_delta[axis] = -1.0f / d;
        } else {
            step[axis] = 0;
            t_max[axis] = INFINITY;
            t_delta[axis] = INFINITY;
        }
        guard += labs(end[axis] - cell[axis]);
    }

    const size_t nxy = grid->nx * grid->ny;
    size_t index = ((size_t)cell[2] * nxy) + ((size_t)cell[1] * grid->nx) + (size_t)cell[0];
    if (grid->cells[index] == (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY) {
        return false;
    }
    while ((cell[0] != end[0] || cell[1] != end[1] || cell[2] != end[2]) && guard-- > 0) {
        const float t_min = fminf(t_max[0], fminf(t_max[1], t_max[2]));
        if (!(t_min <= 1.0f + 1.0e-5f)) {
            break;
        }
        /* An exact edge/corner crossing steps diagonally, as in traverse(). */
        bool tied[3];
        for (size_t axis = 0U; axis < 3U; ++axis) {
            tied[axis] = t_max[axis] <= t_min + 1.0e-6f;
            const long next = cell[axis] + (tied[axis] ? step[axis] : 0);
            if (next < 0 || next >= n[axis]) {
                return false;
            }
        }
        if (tie_sealed(grid, cell, step, tied)) {
            return false;
        }
        for (size_t axis = 0U; axis < 3U; ++axis) {
            if (tied[axis]) {
                cell[axis] += step[axis];
                t_max[axis] += t_delta[axis];
            }
        }
        index = ((size_t)cell[2] * nxy) + ((size_t)cell[1] * grid->nx) + (size_t)cell[0];
        if (grid->cells[index] == (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY) {
            return false;
        }
    }
    return true;
}


static void heap_swap(suea_search_t *search, size_t a, size_t b)
{
    const int32_t va = search->heap[a];
    const int32_t vb = search->heap[b];
    search->heap[a] = vb;
    search->heap[b] = va;
    search->heap_position[vb] = (int32_t)a;
    search->heap_position[va] = (int32_t)b;
}


static void heap_up(suea_search_t *search, size_t i)
{
    const float *key = search->field->length_m;
    while (i > 0U) {
        const size_t parent = (i - 1U) / 2U;
        if (!(key[search->heap[i]] < key[search->heap[parent]])) {
            break;
        }
        heap_swap(search, i, parent);
        i = parent;
    }
}


static void heap_down(suea_search_t *search, size_t i)
{
    const float *key = search->field->length_m;
    for (;;) {
        const size_t left = 2U * i + 1U;
        const size_t right = left + 1U;
        size_t smallest = i;
        if (left < search->heap_size && key[search->heap[left]] < key[search->heap[smallest]]) {
            smallest = left;
        }
        if (right < search->heap_size && key[search->heap[right]] < key[search->heap[smallest]]) {
            smallest = right;
        }
        if (smallest == i) {
            return;
        }
        heap_swap(search, i, smallest);
        i = smallest;
    }
}


static void heap_push(suea_search_t *search, int32_t cell)
{
    search->heap[search->heap_size] = cell;
    search->heap_position[cell] = (int32_t)search->heap_size;
    ++search->heap_size;
    heap_up(search, search->heap_size - 1U);
}


static int32_t heap_pop(suea_search_t *search)
{
    const int32_t top = search->heap[0];
    --search->heap_size;
    if (search->heap_size > 0U) {
        heap_swap(search, 0U, search->heap_size);
        heap_down(search, 0U);
    }
    search->heap_position[top] = -1;
    return top;
}


static bool neighbor_index(
    const localization_suea_grid_t *grid,
    size_t index,
    int dx,
    int dy,
    int dz,
    size_t *neighbor
)
{
    size_t ix = 0U, iy = 0U, iz = 0U;
    cell_coords(grid, index, &ix, &iy, &iz);
    const long x = (long)ix + dx;
    const long y = (long)iy + dy;
    const long z = (long)iz + dz;
    if (x < 0 || y < 0 || z < 0 || x >= (long)grid->nx || y >= (long)grid->ny || z >= (long)grid->nz) {
        return false;
    }
    *neighbor = cell_index(grid, (size_t)x, (size_t)y, (size_t)z);
    return true;
}


static bool snap_source(const localization_suea_grid_t *grid, const localization_point3d_t *source, size_t *start)
{
    size_t ix = 0U, iy = 0U, iz = 0U;
    if (!localization_suea_grid_locate(grid, source, &ix, &iy, &iz)) {
        return false;
    }
    const size_t own = cell_index(grid, ix, iy, iz);
    if (grid->cells[own] != (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY) {
        *start = own;
        return true;
    }

    bool found = false;
    double best = INFINITY;
    for (int dz = -SUEA_SNAP_RADIUS_CELLS; dz <= SUEA_SNAP_RADIUS_CELLS; ++dz) {
        for (int dy = -SUEA_SNAP_RADIUS_CELLS; dy <= SUEA_SNAP_RADIUS_CELLS; ++dy) {
            for (int dx = -SUEA_SNAP_RADIUS_CELLS; dx <= SUEA_SNAP_RADIUS_CELLS; ++dx) {
                size_t candidate = 0U;
                if (
                    neighbor_index(grid, own, dx, dy, dz, &candidate) &&
                    grid->cells[candidate] != (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY
                ) {
                    const double distance = vf_distance(*source, index_center(grid, candidate));
                    if (distance < best) {
                        best = distance;
                        *start = candidate;
                        found = true;
                    }
                }
            }
        }
    }
    return found;
}


esp_err_t localization_suea_field_compute(
    const localization_suea_grid_t *grid,
    const localization_point3d_t *source,
    localization_suea_field_t *field
)
{
    if (grid == NULL || grid->cells == NULL || source == NULL || field == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(field, 0, sizeof(*field));

    const size_t count = localization_suea_grid_cell_count(grid);
    if (count == 0U || count > (size_t)INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t start = 0U;
    if (!snap_source(grid, source, &start)) {
        return ESP_ERR_NOT_FOUND;
    }

    field->source = *source;
    field->count = count;
    field->length_m = malloc(count * sizeof(float));
    field->parent = malloc(count * sizeof(int32_t));
    suea_search_t search = {
        .grid = grid,
        .field = field,
        .heap = malloc(count * sizeof(int32_t)),
        .heap_position = malloc(count * sizeof(int32_t)),
        .state = calloc(count, 1U),
        .source = {
            (float)((source->x_m - grid->origin.x_m) / grid->cell_m),
            (float)((source->y_m - grid->origin.y_m) / grid->cell_m),
            (float)((source->z_m - grid->origin.z_m) / grid->cell_m),
        },
    };
    if (
        field->length_m == NULL || field->parent == NULL ||
        search.heap == NULL || search.heap_position == NULL || search.state == NULL
    ) {
        free(search.heap);
        free(search.heap_position);
        free(search.state);
        localization_suea_field_free(field);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0U; i < count; ++i) {
        field->length_m[i] = INFINITY;
        field->parent[i] = SUEA_NO_PARENT;
        search.heap_position[i] = -1;
    }

    /* Grid-unit lengths while searching (the heap key); meters at the end. */
    float start_center[3];
    cell_center_grid(grid, start, start_center);
    field->length_m[start] = distance_grid(search.source, start_center);
    field->parent[start] = SUEA_SOURCE_PARENT;
    search.state[start] = SUEA_STATE_OPEN;
    heap_push(&search, (int32_t)start);

    const long nx = (long)grid->nx;
    const long ny = (long)grid->ny;
    const long nz = (long)grid->nz;
    const long nxy = nx * ny;

    while (search.heap_size > 0U) {
        const int32_t current = heap_pop(&search);
        size_t cx = 0U, cy = 0U, cz = 0U;
        cell_coords(grid, (size_t)current, &cx, &cy, &cz);
        const float center[3] = { (float)cx + 0.5f, (float)cy + 0.5f, (float)cz + 0.5f };

        /* Lazy Theta* SetVertex: verify the optimistic any-angle parent,
         * else fall back to the best already-closed neighbor. */
        const int32_t assumed = field->parent[current];
        float assumed_point[3];
        vertex_grid(&search, assumed, assumed_point);
        if ((size_t)current != start && !line_of_sight_grid(grid, assumed_point, center)) {
            float best = INFINITY;
            int32_t best_parent = SUEA_NO_PARENT;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const long x = (long)cx + dx, y = (long)cy + dy, z = (long)cz + dz;
                        if (
                            (dx == 0 && dy == 0 && dz == 0) ||
                            x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz
                        ) {
                            continue;
                        }
                        const size_t neighbor = (size_t)(z * nxy + y * nx + x);
                        if (search.state[neighbor] != SUEA_STATE_CLOSED) {
                            continue;
                        }
                        const float neighbor_center[3] = { (float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f };
                        const float candidate = field->length_m[neighbor] + distance_grid(neighbor_center, center);
                        if (candidate < best && line_of_sight_grid(grid, neighbor_center, center)) {
                            best = candidate;
                            best_parent = (int32_t)neighbor;
                        }
                    }
                }
            }
            if (best_parent == SUEA_NO_PARENT) {
                field->length_m[current] = INFINITY;
                field->parent[current] = SUEA_NO_PARENT;
                search.state[current] = SUEA_STATE_CLOSED;
                continue;
            }
            field->length_m[current] = best;
            field->parent[current] = best_parent;
        }
        search.state[current] = SUEA_STATE_CLOSED;

        const int32_t parent = field->parent[current];
        float parent_point[3];
        vertex_grid(&search, parent, parent_point);
        const float parent_length = vertex_length(&search, parent);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const long x = (long)cx + dx, y = (long)cy + dy, z = (long)cz + dz;
                    if (
                        (dx == 0 && dy == 0 && dz == 0) ||
                        x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz
                    ) {
                        continue;
                    }
                    const size_t neighbor = (size_t)(z * nxy + y * nx + x);
                    if (
                        search.state[neighbor] == SUEA_STATE_CLOSED ||
                        grid->cells[neighbor] == (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY
                    ) {
                        continue;
                    }
                    const float neighbor_center[3] = { (float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f };
                    const float candidate = parent_length + distance_grid(parent_point, neighbor_center);
                    if (search.state[neighbor] == SUEA_STATE_NEW) {
                        field->length_m[neighbor] = candidate;
                        field->parent[neighbor] = parent;
                        search.state[neighbor] = SUEA_STATE_OPEN;
                        heap_push(&search, (int32_t)neighbor);
                    } else if (candidate < field->length_m[neighbor]) {
                        field->length_m[neighbor] = candidate;
                        field->parent[neighbor] = parent;
                        heap_up(&search, (size_t)search.heap_position[neighbor]);
                    }
                }
            }
        }
    }

    const float cell_m = (float)grid->cell_m;
    for (size_t i = 0U; i < count; ++i) {
        if (search.state[i] != SUEA_STATE_CLOSED || !isfinite(field->length_m[i])) {
            field->length_m[i] = INFINITY;
            field->parent[i] = SUEA_NO_PARENT;
        } else {
            field->length_m[i] *= cell_m;
        }
    }
    free(search.heap);
    free(search.heap_position);
    free(search.state);
    return ESP_OK;
}


void localization_suea_field_free(localization_suea_field_t *field)
{
    if (field == NULL) {
        return;
    }
    free(field->length_m);
    free(field->parent);
    memset(field, 0, sizeof(*field));
}


double localization_suea_field_length_at(
    const localization_suea_grid_t *grid,
    const localization_suea_field_t *field,
    const localization_point3d_t *point
)
{
    size_t ix = 0U, iy = 0U, iz = 0U;
    if (
        grid == NULL || field == NULL || field->length_m == NULL || point == NULL ||
        !localization_suea_grid_locate(grid, point, &ix, &iy, &iz)
    ) {
        return INFINITY;
    }
    const size_t cell = cell_index(grid, ix, iy, iz);
    if (
        grid->cells[cell] == (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY ||
        !isfinite(field->length_m[cell])
    ) {
        return INFINITY;
    }

    const int32_t parent = field->parent[cell];
    const localization_point3d_t parent_point =
        parent < 0 ? field->source : index_center(grid, (size_t)parent);
    const double parent_length = parent < 0 ? 0.0 : (double)field->length_m[parent];
    if (parent != SUEA_NO_PARENT && line_of_sight(grid, parent_point, *point)) {
        return parent_length + vf_distance(parent_point, *point);
    }
    return (double)field->length_m[cell] + vf_distance(index_center(grid, cell), *point);
}


/* ---- Location --------------------------------------------------------- */

typedef struct
{
    size_t count;
    size_t pick_index[LOCALIZATION_SUEA_MAX_SENSORS];
    localization_suea_field_t field[LOCALIZATION_SUEA_MAX_SENSORS];
    localization_point3d_t sensor[LOCALIZATION_SUEA_MAX_SENSORS];
    double arrival_s[LOCALIZATION_SUEA_MAX_SENSORS];
    double weight[LOCALIZATION_SUEA_MAX_SENSORS];
    double sigma_s;
    double prior_slowness;
    double prior_sigma;

} suea_problem_t;


static void free_problem(suea_problem_t *problem)
{
    for (size_t i = 0U; i < LOCALIZATION_SUEA_MAX_SENSORS; ++i) {
        localization_suea_field_free(&problem->field[i]);
    }
}


static bool fit_at(
    const suea_problem_t *problem,
    const double *lengths,
    const localization_velocity_bounds_t *bounds,
    vf_time_fit_t *fit
)
{
    if (bounds != NULL && bounds->set) {
        if (bounds->v_min_mps == bounds->v_max_mps) {
            return vf_fit_travel_times(
                problem->arrival_s, lengths, problem->weight, problem->count,
                problem->sigma_s, 1.0 / bounds->v_min_mps, 0.0, fit
            ) && fit->slowness_s_per_m > 0.0;
        }
        return vf_fit_travel_times_bounded(
            problem->arrival_s, lengths, problem->weight, problem->count,
            problem->sigma_s, 0.5 * (bounds->v_min_mps + bounds->v_max_mps),
            bounds, fit, NULL
        ) && fit->slowness_s_per_m > 0.0;
    }
    return
        vf_fit_travel_times(
            problem->arrival_s, lengths, problem->weight, problem->count,
            problem->sigma_s, problem->prior_slowness, problem->prior_sigma, fit
        ) &&
        fit->slowness_s_per_m > 0.0;
}


esp_err_t localization_suea_locate(
    const localization_suea_grid_t *grid,
    const localization_suea_pick_t *picks,
    size_t pick_count,
    const localization_suea_config_t *config,
    localization_suea_result_t *result
)
{
    if (
        grid == NULL || grid->cells == NULL || picks == NULL || config == NULL || result == NULL ||
        pick_count == 0U || pick_count > LOCALIZATION_SUEA_MAX_SENSORS ||
        !(config->timing_uncertainty_us > 0.0) || !isfinite(config->timing_uncertainty_us) ||
        config->refine_divisor < 2U ||
        !isfinite(config->prior_velocity_mps) || !isfinite(config->prior_velocity_sigma_mps) ||
        config->prior_velocity_sigma_mps < 0.0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const localization_velocity_bounds_t *bounds = config->velocity_bounds;
    const bool bounds_set = bounds != NULL && bounds->set;
    if (bounds_set && localization_velocity_bounds_validate(bounds) != LOCALIZATION_VELOCITY_BOUNDS_VALID) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->cell_m_used = grid->cell_m;
    result->empty_cell_count = localization_suea_grid_count(grid, LOCALIZATION_SUEA_CELL_EMPTY);

    suea_problem_t problem;
    memset(&problem, 0, sizeof(problem));
    problem.sigma_s = config->timing_uncertainty_us * 1.0e-6;
    if (bounds_set && bounds->v_min_mps == bounds->v_max_mps) {
        problem.prior_slowness = 1.0 / bounds->v_min_mps;
        problem.prior_sigma = 0.0;
    } else if (bounds_set) {
        problem.prior_slowness = 0.0;
        problem.prior_sigma = -1.0;
    } else if (config->prior_velocity_mps > 0.0) {
        problem.prior_slowness = 1.0 / config->prior_velocity_mps;
        /* Delta method: sigma(1/v) = sigma_v / v^2. */
        problem.prior_sigma =
            config->prior_velocity_sigma_mps / (config->prior_velocity_mps * config->prior_velocity_mps);
    } else {
        problem.prior_slowness = 0.0;
        problem.prior_sigma = -1.0;
    }

    int64_t first_us = 0;
    bool have_first = false;
    for (size_t i = 0U; i < pick_count; ++i) {
        const localization_point3d_t p = picks[i].sensor_position;
        if (!(picks[i].weight > 0.0) || !isfinite(p.x_m) || !isfinite(p.y_m) || !isfinite(p.z_m)) {
            continue;
        }
        const esp_err_t err = localization_suea_field_compute(grid, &p, &problem.field[problem.count]);
        if (err == ESP_ERR_NOT_FOUND) {
            result->pick_mask_unreachable |= 1U << i;
            continue;
        }
        if (err != ESP_OK) {
            free_problem(&problem);
            return err;
        }
        problem.pick_index[problem.count] = i;
        problem.sensor[problem.count] = p;
        problem.weight[problem.count] = picks[i].weight;
        if (!have_first || picks[i].arrival_time_us < first_us) {
            first_us = picks[i].arrival_time_us;
            have_first = true;
        }
        result->pick_mask_used |= 1U << i;
        ++problem.count;
    }
    result->picks_used_count = problem.count;
    if (problem.count < SUEA_MIN_PICKS) {
        result->status = LOCALIZATION_SUEA_INSUFFICIENT_PICKS;
        result->velocity_status = bounds_set
            ? LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE
            : LOCALIZATION_VELOCITY_STATUS_UNKNOWN;
        free_problem(&problem);
        return ESP_OK;
    }
    for (size_t k = 0U; k < problem.count; ++k) {
        problem.arrival_s[k] = (double)(picks[problem.pick_index[k]].arrival_time_us - first_us) * 1.0e-6;
    }

    /* Coarse pass: every passable cell center. */
    size_t reachable = 0U;
    size_t allowed = 0U;
    bool found = false;
    double best_chi = INFINITY;
    localization_point3d_t best = { 0.0, 0.0, 0.0 };
    vf_time_fit_t best_fit;
    memset(&best_fit, 0, sizeof(best_fit));
    double lengths[LOCALIZATION_SUEA_MAX_SENSORS];

    const size_t total = localization_suea_grid_cell_count(grid);
    for (size_t cell = 0U; cell < total; ++cell) {
        if (grid->cells[cell] == (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY) {
            continue;
        }
        bool finite = true;
        for (size_t k = 0U; k < problem.count && finite; ++k) {
            lengths[k] = (double)problem.field[k].length_m[cell];
            finite = isfinite(lengths[k]);
        }
        if (!finite) {
            continue;
        }
        ++reachable;
        const localization_point3d_t center = index_center(grid, cell);
        if (!localization_candidate_admitted(&config->candidate_filter, &center)) {
            continue;
        }
        ++allowed;
        ++result->candidates_evaluated;
        vf_time_fit_t fit;
        if (fit_at(&problem, lengths, bounds, &fit) && fit.chi_square < best_chi) {
            best_chi = fit.chi_square;
            best = center;
            best_fit = fit;
            found = true;
        }
    }

    if (!found) {
        result->status = reachable > 0U && allowed == 0U
            ? LOCALIZATION_SUEA_NO_ALLOWED_CANDIDATE
            : LOCALIZATION_SUEA_NO_REACHABLE_CANDIDATE;
        result->velocity_status = bounds_set
            ? (reachable > 0U && allowed == 0U
                ? LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE
                : LOCALIZATION_VELOCITY_STATUS_INCONSISTENT)
            : LOCALIZATION_VELOCITY_STATUS_UNKNOWN;
        free_problem(&problem);
        return ESP_OK;
    }

    /* Refinement: +/- one cell around the coarse best, sub-cell steps. */
    const localization_point3d_t coarse = best;
    const double step = grid->cell_m / (double)config->refine_divisor;
    const long span = (long)config->refine_divisor;
    const long span_z = grid->nz > 1U ? span : 0;
    for (long dz = -span_z; dz <= span_z; ++dz) {
        for (long dy = -span; dy <= span; ++dy) {
            for (long dx = -span; dx <= span; ++dx) {
                const localization_point3d_t candidate = {
                    coarse.x_m + (double)dx * step,
                    coarse.y_m + (double)dy * step,
                    coarse.z_m + (double)dz * step,
                };
                bool finite = true;
                for (size_t k = 0U; k < problem.count && finite; ++k) {
                    lengths[k] = localization_suea_field_length_at(grid, &problem.field[k], &candidate);
                    finite = isfinite(lengths[k]);
                }
                if (!finite || !localization_candidate_admitted(&config->candidate_filter, &candidate)) {
                    continue;
                }
                ++result->candidates_evaluated;
                vf_time_fit_t fit;
                if (fit_at(&problem, lengths, bounds, &fit) && fit.chi_square < best_chi) {
                    best_chi = fit.chi_square;
                    best = candidate;
                    best_fit = fit;
                }
            }
        }
    }

    double detour = 0.0;
    for (size_t k = 0U; k < problem.count; ++k) {
        const double length = localization_suea_field_length_at(grid, &problem.field[k], &best);
        const double straight = vf_distance(best, problem.sensor[k]);
        if (isfinite(length) && length - straight > detour) {
            detour = length - straight;
        }
    }

    result->status = LOCALIZATION_SUEA_SOLVED;
    result->best_position = best;
    result->best_residual_rms_us = best_fit.rms_s * 1.0e6;
    result->velocity_mps = 1.0 / best_fit.slowness_s_per_m;
    result->emission_time_us = first_us + (int64_t)llround(best_fit.t0_s * 1.0e6);
    result->max_detour_m = detour;
    if (bounds_set) {
        const double velocity = result->velocity_mps;
        const bool at_bound =
            bounds->v_min_mps < bounds->v_max_mps &&
                (fabs(velocity - bounds->v_min_mps) <= 0.01 * bounds->v_min_mps ||
                 fabs(bounds->v_max_mps - velocity) <= 0.01 * bounds->v_max_mps);
        result->velocity_status = result->best_residual_rms_us > 3.0 * config->timing_uncertainty_us
            ? LOCALIZATION_VELOCITY_STATUS_INCONSISTENT
            : at_bound
                ? LOCALIZATION_VELOCITY_STATUS_AT_BOUND
                : LOCALIZATION_VELOCITY_STATUS_OK;
    }
    free_problem(&problem);
    return ESP_OK;
}


/* ---- Empty-area identification ---------------------------------------- */

typedef struct
{
    localization_suea_grid_t *grid;
    float *score;
    uint8_t *sensor_votes; /* bit i: a delayed ray to sensor i crosses the cell */
    uint16_t *shot_votes;  /* bit a: a delayed ray from shot a crosses the cell */
    double weight;
    uint8_t sensor_bit;
    uint16_t shot_bit;
    size_t certified;
    bool certify;

} suea_mark_context_t;


static bool visit_mark(size_t index, void *context)
{
    suea_mark_context_t *mark = context;
    if (mark->grid->cells[index] != (uint8_t)LOCALIZATION_SUEA_CELL_UNKNOWN) {
        return true;
    }
    if (mark->certify) {
        mark->grid->cells[index] = (uint8_t)LOCALIZATION_SUEA_CELL_SOLID;
        ++mark->certified;
    } else if ((mark->sensor_votes[index] & mark->sensor_bit) == 0U || (mark->shot_votes[index] & mark->shot_bit) == 0U) {
        /* One vote per ray, however many path segments revisit the cell. */
        mark->score[index] += (float)mark->weight;
        mark->sensor_votes[index] |= mark->sensor_bit;
        mark->shot_votes[index] |= mark->shot_bit;
    }
    return true;
}


static unsigned bit_count(uint32_t bits)
{
    unsigned count = 0U;
    for (; bits != 0U; bits &= bits - 1U) {
        ++count;
    }
    return count;
}


/* Walks the current shortest path from `point` back to the field's source
 * (the sensor), visiting every cell on it. */
static void trace_path(
    localization_suea_grid_t *grid,
    const localization_suea_field_t *field,
    localization_point3d_t point,
    suea_mark_context_t *mark
)
{
    size_t ix = 0U, iy = 0U, iz = 0U;
    if (!localization_suea_grid_locate(grid, &point, &ix, &iy, &iz)) {
        return;
    }
    int32_t current = (int32_t)cell_index(grid, ix, iy, iz);
    (void)traverse(grid, point, index_center(grid, (size_t)current), visit_mark, mark);
    for (size_t guard = field->count; guard > 0U; --guard) {
        const int32_t parent = field->parent[current];
        if (parent == SUEA_NO_PARENT) {
            return;
        }
        const localization_point3d_t to = parent < 0 ? field->source : index_center(grid, (size_t)parent);
        (void)traverse(grid, index_center(grid, (size_t)current), to, visit_mark, mark);
        if (parent < 0) {
            return;
        }
        current = parent;
    }
}


typedef struct
{
    double length[LOCALIZATION_SUEA_MAX_SHOTS][LOCALIZATION_SUEA_MAX_SENSORS];
    double arrival_s[LOCALIZATION_SUEA_MAX_SHOTS][LOCALIZATION_SUEA_MAX_SENSORS];
    bool valid[LOCALIZATION_SUEA_MAX_SHOTS][LOCALIZATION_SUEA_MAX_SENSORS];
    double residual[LOCALIZATION_SUEA_MAX_SHOTS][LOCALIZATION_SUEA_MAX_SENSORS];
    double slowness;
    double sse;
    size_t rays;

} suea_shot_model_t;


/* One pass of t_ai = t0_a + s L_ai (one slowness s for the medium, a
 * fitted origin t0_a per shot unless it is known) over the rays in
 * `include`: closed form. Residuals and sse cover EVERY valid ray. */
static bool fit_shots_once(
    const localization_suea_active_shot_t *shots,
    size_t shot_count,
    size_t sensor_count,
    bool include[LOCALIZATION_SUEA_MAX_SHOTS][LOCALIZATION_SUEA_MAX_SENSORS],
    suea_shot_model_t *model
)
{
    double numerator = 0.0;
    double denominator = 0.0;
    double mean_l[LOCALIZATION_SUEA_MAX_SHOTS] = { 0.0 };
    double mean_t[LOCALIZATION_SUEA_MAX_SHOTS] = { 0.0 };
    size_t shot_included[LOCALIZATION_SUEA_MAX_SHOTS] = { 0U };
    size_t included = 0U;

    for (size_t a = 0U; a < shot_count; ++a) {
        size_t n = 0U;
        for (size_t i = 0U; i < sensor_count; ++i) {
            if (include[a][i]) {
                mean_l[a] += model->length[a][i];
                mean_t[a] += model->arrival_s[a][i];
                ++n;
            }
        }
        shot_included[a] = n;
        included += n;
        if (n == 0U) {
            continue;
        }
        mean_l[a] /= (double)n;
        mean_t[a] /= (double)n;
        for (size_t i = 0U; i < sensor_count; ++i) {
            if (!include[a][i]) {
                continue;
            }
            if (shots[a].origin_time_known) {
                numerator += model->length[a][i] * model->arrival_s[a][i];
                denominator += model->length[a][i] * model->length[a][i];
            } else {
                const double dl = model->length[a][i] - mean_l[a];
                numerator += dl * (model->arrival_s[a][i] - mean_t[a]);
                denominator += dl * dl;
            }
        }
    }
    if (included < 2U || !(denominator > 0.0)) {
        return false;
    }
    model->slowness = numerator / denominator;
    if (!(model->slowness > 0.0)) {
        return false;
    }

    model->sse = 0.0;
    model->rays = 0U;
    for (size_t a = 0U; a < shot_count; ++a) {
        /* A shot with no included ray keeps its own mean as the origin. */
        size_t n = 0U;
        double own_l = 0.0;
        double own_t = 0.0;
        for (size_t i = 0U; i < sensor_count; ++i) {
            if (model->valid[a][i]) {
                own_l += model->length[a][i];
                own_t += model->arrival_s[a][i];
                ++n;
            }
        }
        double t0 = 0.0;
        if (!shots[a].origin_time_known) {
            t0 = shot_included[a] > 0U
                ? mean_t[a] - model->slowness * mean_l[a]
                : (n > 0U ? (own_t - model->slowness * own_l) / (double)n : 0.0);
        }
        for (size_t i = 0U; i < sensor_count; ++i) {
            if (model->valid[a][i]) {
                const double r = model->arrival_s[a][i] - t0 - model->slowness * model->length[a][i];
                model->residual[a][i] = r;
                model->sse += r * r;
                ++model->rays;
            }
        }
    }
    return true;
}


/*
 * An empty area can only DELAY a ray, never speed it up, so the medium's
 * slowness is fitted from the rays that are not delayed: fit, drop the rays
 * later than late_s, refit, until the set is stable (one-sided trimming).
 * The reported sse still covers every valid ray -- it is the misfit the
 * empty-area model has to explain.
 */
static bool fit_shots(
    const localization_suea_active_shot_t *shots,
    size_t shot_count,
    size_t sensor_count,
    double late_s,
    suea_shot_model_t *model
)
{
    bool include[LOCALIZATION_SUEA_MAX_SHOTS][LOCALIZATION_SUEA_MAX_SENSORS];
    memcpy(include, model->valid, sizeof(include));
    for (int pass = 0; pass < 8; ++pass) {
        if (!fit_shots_once(shots, shot_count, sensor_count, include, model)) {
            return false;
        }
        bool changed = false;
        for (size_t a = 0U; a < shot_count; ++a) {
            for (size_t i = 0U; i < sensor_count; ++i) {
                const bool keep = model->valid[a][i] && model->residual[a][i] <= late_s;
                changed = changed || keep != include[a][i];
                include[a][i] = keep;
            }
        }
        if (!changed) {
            break;
        }
    }
    return true;
}


/* Recomputes every sensor's paths through the current grid and refits the
 * shot model. False when the model cannot be fitted; *err reports an
 * allocation failure. */
static bool evaluate_model(
    const localization_suea_grid_t *grid,
    const localization_point3d_t *sensor_positions,
    size_t sensor_count,
    const localization_suea_active_shot_t *shots,
    size_t shot_count,
    double late_s,
    localization_suea_field_t *fields,
    suea_shot_model_t *model,
    esp_err_t *err
)
{
    bool sensor_ok[LOCALIZATION_SUEA_MAX_SENSORS];
    for (size_t i = 0U; i < sensor_count; ++i) {
        localization_suea_field_free(&fields[i]);
        const esp_err_t field_err = localization_suea_field_compute(grid, &sensor_positions[i], &fields[i]);
        if (field_err == ESP_ERR_NO_MEM) {
            *err = field_err;
            return false;
        }
        sensor_ok[i] = field_err == ESP_OK;
    }
    for (size_t a = 0U; a < shot_count; ++a) {
        const double origin = shots[a].origin_time_known ? (double)shots[a].origin_time_us : 0.0;
        for (size_t i = 0U; i < sensor_count; ++i) {
            model->valid[a][i] = false;
            if (!shots[a].arrival_valid[i] || !sensor_ok[i]) {
                continue;
            }
            const double length = localization_suea_field_length_at(grid, &fields[i], &shots[a].position);
            if (!isfinite(length)) {
                continue;
            }
            model->length[a][i] = length;
            model->arrival_s[a][i] = ((double)shots[a].arrival_time_us[i] - origin) * 1.0e-6;
            model->valid[a][i] = true;
        }
    }
    return fit_shots(shots, shot_count, sensor_count, late_s, model);
}


/* An EMPTY cell with a non-EMPTY face neighbor: the edge of a void. */
static bool on_empty_boundary(const localization_suea_grid_t *grid, size_t index)
{
    static const int faces[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (size_t f = 0U; f < 6U; ++f) {
        size_t neighbor = 0U;
        if (
            neighbor_index(grid, index, faces[f][0], faces[f][1], faces[f][2], &neighbor) &&
            grid->cells[neighbor] != (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY
        ) {
            return true;
        }
    }
    return false;
}


esp_err_t localization_suea_identify_empty_areas(
    localization_suea_grid_t *grid,
    const localization_point3d_t *sensor_positions,
    size_t sensor_count,
    const localization_suea_active_shot_t *shots,
    size_t shot_count,
    const localization_suea_identify_config_t *config,
    localization_suea_identify_result_t *result
)
{
    if (
        grid == NULL || grid->cells == NULL || sensor_positions == NULL || shots == NULL ||
        config == NULL || result == NULL ||
        sensor_count == 0U || sensor_count > LOCALIZATION_SUEA_MAX_SENSORS ||
        shot_count == 0U || shot_count > LOCALIZATION_SUEA_MAX_SHOTS ||
        !(config->timing_uncertainty_us > 0.0) ||
        !(config->on_time_threshold_sigma > 0.0) ||
        !(config->late_threshold_sigma > config->on_time_threshold_sigma) ||
        config->max_iterations == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    const size_t total = localization_suea_grid_cell_count(grid);
    float *score = malloc(total * sizeof(float));
    uint8_t *sensor_votes = malloc(total * sizeof(uint8_t));
    uint16_t *shot_votes = malloc(total * sizeof(uint16_t));
    size_t *accepted = malloc(total * sizeof(size_t));
    suea_shot_model_t *model = calloc(1U, sizeof(*model));
    localization_suea_field_t fields[LOCALIZATION_SUEA_MAX_SENSORS];
    memset(fields, 0, sizeof(fields));
    if (score == NULL || sensor_votes == NULL || shot_votes == NULL || accepted == NULL || model == NULL) {
        free(score);
        free(sensor_votes);
        free(shot_votes);
        free(accepted);
        free(model);
        return ESP_ERR_NO_MEM;
    }
    /* A void is corroborated by delayed rays of different sensors AND
     * different shots; cells near one sensor or one shot, where that
     * sensor's/shot's ray fan converges, are not. */
    const unsigned min_sensor_votes = sensor_count >= 2U ? 2U : 1U;
    const unsigned min_shot_votes = shot_count >= 2U ? 2U : 1U;
    const double sigma_s = config->timing_uncertainty_us * 1.0e-6;
    const double late_s = config->late_threshold_sigma * sigma_s;

    /* accepted[0, accepted_count) holds every cell kept EMPTY, in marking
     * order; an attempt's marks follow it until the attempt is judged. */
    size_t accepted_count = 0U;
    esp_err_t err = ESP_OK;

    const bool initial_fit = evaluate_model(
        grid, sensor_positions, sensor_count, shots, shot_count, late_s, fields, model, &err
    );
    double best_sse = initial_fit ? model->sse : INFINITY;
    if (initial_fit) {
        result->rms_before_us = sqrt(model->sse / (double)model->rays) * 1.0e6;
    }

    for (uint32_t round = 1U; err == ESP_OK && initial_fit && round <= config->max_iterations; ++round) {
        /* The fields and model describe the current grid here. On-time rays
         * certify their path; delayed rays vote for voids. */
        suea_mark_context_t mark = {
            .grid = grid, .score = score, .sensor_votes = sensor_votes, .shot_votes = shot_votes,
        };
        bool any_late = false;
        for (size_t a = 0U; a < shot_count; ++a) {
            for (size_t i = 0U; i < sensor_count; ++i) {
                if (model->valid[a][i] && fabs(model->residual[a][i]) <= config->on_time_threshold_sigma * sigma_s) {
                    mark.certify = true;
                    trace_path(grid, &fields[i], shots[a].position, &mark);
                }
                any_late = any_late || (model->valid[a][i] && model->residual[a][i] > late_s);
            }
        }
        result->cells_certified_solid += mark.certified;
        if (!any_late) {
            break;
        }

        memset(score, 0, total * sizeof(float));
        memset(sensor_votes, 0, total * sizeof(uint8_t));
        memset(shot_votes, 0, total * sizeof(uint16_t));
        mark.certify = false;
        for (size_t a = 0U; a < shot_count; ++a) {
            for (size_t i = 0U; i < sensor_count; ++i) {
                if (model->valid[a][i] && model->residual[a][i] > late_s) {
                    mark.weight = model->residual[a][i] / sigma_s;
                    mark.sensor_bit = (uint8_t)(1U << i);
                    mark.shot_bit = (uint16_t)(1U << a);
                    trace_path(grid, &fields[i], shots[a].position, &mark);
                }
            }
        }
        float max_score = 0.0f;
        for (size_t c = 0U; c < total; ++c) {
            if (
                score[c] > max_score &&
                bit_count(sensor_votes[c]) >= min_sensor_votes &&
                bit_count(shot_votes[c]) >= min_shot_votes
            ) {
                max_score = score[c];
            }
        }
        if (!(max_score > 0.0f)) {
            break;
        }

        bool kept = false;
        const size_t fraction_count = sizeof(SUEA_IDENTIFY_SCORE_FRACTIONS) / sizeof(SUEA_IDENTIFY_SCORE_FRACTIONS[0]);
        for (size_t f = 0U; !kept && err == ESP_OK && f < fraction_count; ++f) {
            size_t attempt_count = 0U;
            for (size_t c = 0U; c < total; ++c) {
                if (
                    grid->cells[c] == (uint8_t)LOCALIZATION_SUEA_CELL_UNKNOWN &&
                    bit_count(sensor_votes[c]) >= min_sensor_votes &&
                    bit_count(shot_votes[c]) >= min_shot_votes &&
                    score[c] >= SUEA_IDENTIFY_SCORE_FRACTIONS[f] * max_score
                ) {
                    grid->cells[c] = (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY;
                    accepted[accepted_count + attempt_count++] = c;
                }
            }
            if (attempt_count == 0U) {
                continue;
            }
            const bool fitted = evaluate_model(
                grid, sensor_positions, sensor_count, shots, shot_count, late_s, fields, model, &err
            );
            if (err == ESP_OK && fitted && model->sse < best_sse * (1.0 - SUEA_MIN_IDENTIFY_IMPROVEMENT)) {
                best_sse = model->sse;
                accepted_count += attempt_count;
                kept = true;
            } else {
                /* Did not explain the delays better: undo the attempt. */
                for (size_t k = 0U; k < attempt_count; ++k) {
                    grid->cells[accepted[accepted_count + k]] = (uint8_t)LOCALIZATION_SUEA_CELL_UNKNOWN;
                }
            }
        }
        if (!kept) {
            break;
        }
        result->iterations = round;
    }

    /*
     * Backward elimination: rounds accept marks in batches, so cells that a
     * detour merely hugged ride along. Latest first, each cell on the edge
     * of an identified area is unmarked and stays unmarked when the misfit
     * does not rise -- only cells the travel times actually need survive.
     * Interior cells are never tested (unmarking one changes no path).
     */
    for (int pass = 0; err == ESP_OK && pass < 2; ++pass) {
        size_t tests = 0U;
        for (size_t k = accepted_count; k-- > 0U && tests < SUEA_MAX_PRUNE_TESTS;) {
            const size_t c = accepted[k];
            if (grid->cells[c] != (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY || !on_empty_boundary(grid, c)) {
                continue;
            }
            ++tests;
            grid->cells[c] = (uint8_t)LOCALIZATION_SUEA_CELL_UNKNOWN;
            const bool fitted = evaluate_model(
                grid, sensor_positions, sensor_count, shots, shot_count, late_s, fields, model, &err
            );
            if (err != ESP_OK) {
                grid->cells[c] = (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY;
                break;
            }
            if (fitted && model->sse <= best_sse * (1.0 + SUEA_MIN_IDENTIFY_IMPROVEMENT)) {
                if (model->sse < best_sse) {
                    best_sse = model->sse;
                }
                ++result->cells_pruned;
            } else {
                grid->cells[c] = (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY;
            }
        }
    }

    /* Report the final model. */
    if (err == ESP_OK && result->rms_before_us > 0.0) {
        if (evaluate_model(grid, sensor_positions, sensor_count, shots, shot_count, late_s, fields, model, &err)) {
            result->rms_after_us = sqrt(model->sse / (double)model->rays) * 1.0e6;
            result->velocity_mps = 1.0 / model->slowness;
            result->rays_used = model->rays;
        }
    }
    for (size_t k = 0U; k < accepted_count; ++k) {
        if (grid->cells[accepted[k]] == (uint8_t)LOCALIZATION_SUEA_CELL_EMPTY) {
            ++result->cells_marked_empty;
        }
    }

    for (size_t i = 0U; i < sensor_count; ++i) {
        localization_suea_field_free(&fields[i]);
    }
    free(score);
    free(sensor_votes);
    free(shot_votes);
    free(accepted);
    free(model);
    return err;
}
