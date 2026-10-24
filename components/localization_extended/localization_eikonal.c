#include "localization_eikonal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "localization_ext_fit.h"
#include "localization_vf_math.h"


/* Nodes within this many cells of the source, in line of sight of it, get
 * straight-ray times (the marching scheme is least accurate where the
 * wavefront curves fastest: 2 cells leave ~3.5% error at 0.5 m, 5 cells
 * ~0.7%). */
#define EIKONAL_SOURCE_RADIUS_CELLS 5.0
#define EIKONAL_ROBUST_ROUNDS 2U
#define EIKONAL_MAX_FACTOR 4096U

enum
{
    NODE_FAR = 0,
    NODE_TRIAL = 1,
    NODE_KNOWN = 2
};


/* ---- Model ---------------------------------------------------------- */

static size_t axis_nodes(double min, double max, double step, size_t limit)
{
    if (max <= min) {
        return 1U;
    }
    const double intervals = ceil((max - min) / step - 1.0e-9);
    if (!isfinite(intervals) || intervals < 0.0 || intervals >= (double)limit) {
        return 0U;
    }
    return (size_t)intervals + 1U;
}


esp_err_t localization_eikonal_model_init(
    localization_eikonal_model_t *model,
    const localization_point3d_t *min_corner,
    const localization_point3d_t *max_corner,
    double cell_m,
    size_t max_nodes,
    double background_velocity_mps
)
{
    if (
        model == NULL || min_corner == NULL || max_corner == NULL ||
        !(cell_m > 0.0) || !isfinite(cell_m) ||
        !(background_velocity_mps > 0.0) || !isfinite(background_velocity_mps) ||
        !isfinite(min_corner->x_m) || !isfinite(min_corner->y_m) || !isfinite(min_corner->z_m) ||
        !isfinite(max_corner->x_m) || !isfinite(max_corner->y_m) || !isfinite(max_corner->z_m) ||
        min_corner->x_m > max_corner->x_m || min_corner->y_m > max_corner->y_m ||
        min_corner->z_m > max_corner->z_m
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(model, 0, sizeof(*model));
    if (max_nodes == 0U) {
        max_nodes = LOCALIZATION_EIKONAL_DEFAULT_MAX_NODES;
    }
    if (max_nodes > SIZE_MAX / sizeof(float)) {
        max_nodes = SIZE_MAX / sizeof(float);
    }
    for (size_t factor = 1U; factor <= EIKONAL_MAX_FACTOR; ++factor) {
        const double step = cell_m * (double)factor;
        if (!isfinite(step)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t nx = axis_nodes(min_corner->x_m, max_corner->x_m, step, max_nodes);
        const size_t ny = axis_nodes(min_corner->y_m, max_corner->y_m, step, max_nodes);
        const size_t nz = axis_nodes(min_corner->z_m, max_corner->z_m, step, max_nodes);
        if (nx == 0U || ny == 0U || nz == 0U ||
            nx > max_nodes / ny || nx * ny > max_nodes / nz) {
            continue;
        }
        const size_t count = nx * ny * nz;
        model->slowness_s_per_m = malloc(count * sizeof(float));
        if (model->slowness_s_per_m == NULL) {
            return ESP_ERR_NO_MEM;
        }
        model->origin = *min_corner;
        model->cell_m = step;
        model->resolution_limited = factor > 1U;
        model->nx = nx;
        model->ny = ny;
        model->nz = nz;
        const float slowness = (float)(1.0 / background_velocity_mps);
        for (size_t i = 0U; i < count; ++i) {
            model->slowness_s_per_m[i] = slowness;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_SIZE;
}


void localization_eikonal_model_free(localization_eikonal_model_t *model)
{
    if (model != NULL) {
        free(model->slowness_s_per_m);
        memset(model, 0, sizeof(*model));
    }
}


size_t localization_eikonal_node_count(const localization_eikonal_model_t *model)
{
    return model->nx * model->ny * model->nz;
}


localization_point3d_t localization_eikonal_node_position(
    const localization_eikonal_model_t *model,
    size_t index
)
{
    const size_t ix = index % model->nx;
    const size_t iy = (index / model->nx) % model->ny;
    const size_t iz = index / (model->nx * model->ny);
    const localization_point3d_t point = {
        model->origin.x_m + (double)ix * model->cell_m,
        model->origin.y_m + (double)iy * model->cell_m,
        model->origin.z_m + (double)iz * model->cell_m,
    };
    return point;
}


void localization_eikonal_set_velocity(localization_eikonal_model_t *model, size_t index, double velocity_mps)
{
    model->slowness_s_per_m[index] =
        velocity_mps > 0.0 && isfinite(velocity_mps) ? (float)(1.0 / velocity_mps) : INFINITY;
}


double localization_eikonal_velocity(const localization_eikonal_model_t *model, size_t index)
{
    const float slowness = model->slowness_s_per_m[index];
    return isfinite(slowness) && slowness > 0.0f ? 1.0 / (double)slowness : 0.0;
}


void localization_eikonal_model_assign(
    localization_eikonal_model_t *model,
    localization_eikonal_velocity_fn velocity,
    void *context
)
{
    const size_t count = localization_eikonal_node_count(model);
    for (size_t i = 0U; i < count; ++i) {
        const localization_point3d_t point = localization_eikonal_node_position(model, i);
        localization_eikonal_set_velocity(model, i, velocity(&point, context));
    }
}


size_t localization_eikonal_passable_count(const localization_eikonal_model_t *model)
{
    size_t count = 0U;
    const size_t total = localization_eikonal_node_count(model);
    for (size_t i = 0U; i < total; ++i) {
        count += isfinite(model->slowness_s_per_m[i]) ? 1U : 0U;
    }
    return count;
}


/* ---- Fast marching -------------------------------------------------- */

typedef struct
{
    int32_t *heap;
    int32_t *position;     /* node -> heap slot, -1 when absent */
    size_t size;
    const float *key;

} node_heap_t;


static void heap_swap(node_heap_t *h, size_t a, size_t b)
{
    const int32_t node_a = h->heap[a];
    h->heap[a] = h->heap[b];
    h->heap[b] = node_a;
    h->position[h->heap[a]] = (int32_t)a;
    h->position[h->heap[b]] = (int32_t)b;
}


static void heap_up(node_heap_t *h, size_t slot)
{
    while (slot > 0U) {
        const size_t parent = (slot - 1U) / 2U;
        if (h->key[h->heap[parent]] <= h->key[h->heap[slot]]) {
            break;
        }
        heap_swap(h, slot, parent);
        slot = parent;
    }
}


static void heap_down(node_heap_t *h, size_t slot)
{
    for (;;) {
        const size_t left = 2U * slot + 1U;
        const size_t right = left + 1U;
        size_t smallest = slot;
        if (left < h->size && h->key[h->heap[left]] < h->key[h->heap[smallest]]) {
            smallest = left;
        }
        if (right < h->size && h->key[h->heap[right]] < h->key[h->heap[smallest]]) {
            smallest = right;
        }
        if (smallest == slot) {
            return;
        }
        heap_swap(h, slot, smallest);
        slot = smallest;
    }
}


static void heap_push_or_decrease(node_heap_t *h, int32_t node)
{
    if (h->position[node] < 0) {
        h->heap[h->size] = node;
        h->position[node] = (int32_t)h->size;
        ++h->size;
    }
    heap_up(h, (size_t)h->position[node]);
}


static int32_t heap_pop(node_heap_t *h)
{
    const int32_t top = h->heap[0];
    --h->size;
    if (h->size > 0U) {
        h->heap[0] = h->heap[h->size];
        h->position[h->heap[0]] = 0;
        heap_down(h, 0U);
    }
    h->position[top] = -1;
    return top;
}


/* Upwind update of one node from its KNOWN neighbours: second order along
 * an axis where two known nodes line up (and decrease towards the node),
 * first order otherwise. */
static float solve_node(
    const localization_eikonal_model_t *model,
    const float *times,
    const uint8_t *state,
    size_t ix,
    size_t iy,
    size_t iz
)
{
    const size_t counts[3] = { model->nx, model->ny, model->nz };
    const size_t coordinate[3] = { ix, iy, iz };
    const size_t stride[3] = { 1U, model->nx, model->nx * model->ny };
    const size_t index = ix + stride[1] * iy + stride[2] * iz;
    const double sh = (double)model->slowness_s_per_m[index] * model->cell_m;

    double alpha[3];
    double beta[3];
    size_t axes = 0U;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (counts[axis] < 2U) {
            continue;
        }
        double t1 = INFINITY;
        double t2 = INFINITY;
        for (int direction = -1; direction <= 1; direction += 2) {
            const long c1 = (long)coordinate[axis] + direction;
            if (c1 < 0 || c1 >= (long)counts[axis]) {
                continue;
            }
            const size_t n1 = (size_t)((long)index + direction * (long)stride[axis]);
            if (state[n1] != NODE_KNOWN || !((double)times[n1] < t1)) {
                continue;
            }
            t1 = times[n1];
            t2 = INFINITY;
            const long c2 = c1 + direction;
            if (c2 >= 0 && c2 < (long)counts[axis]) {
                const size_t n2 = (size_t)((long)n1 + direction * (long)stride[axis]);
                if (state[n2] == NODE_KNOWN && times[n2] <= times[n1]) {
                    t2 = times[n2];
                }
            }
        }
        if (!isfinite(t1)) {
            continue;
        }
        if (isfinite(t2)) {
            alpha[axes] = 2.25;
            beta[axes] = (4.0 * t1 - t2) / 3.0;
        } else {
            alpha[axes] = 1.0;
            beta[axes] = t1;
        }
        /* Keep sorted by beta. */
        for (size_t k = axes; k > 0U && beta[k - 1U] > beta[k]; --k) {
            const double a = alpha[k];
            const double b = beta[k];
            alpha[k] = alpha[k - 1U];
            beta[k] = beta[k - 1U];
            alpha[k - 1U] = a;
            beta[k - 1U] = b;
        }
        ++axes;
    }
    if (axes == 0U) {
        return INFINITY;
    }

    double t = beta[0] + sh / sqrt(alpha[0]);
    double sum_a = alpha[0];
    double sum_ab = alpha[0] * beta[0];
    double sum_abb = alpha[0] * beta[0] * beta[0];
    for (size_t m = 1U; m < axes; ++m) {
        if (t <= beta[m]) {
            break;
        }
        sum_a += alpha[m];
        sum_ab += alpha[m] * beta[m];
        sum_abb += alpha[m] * beta[m] * beta[m];
        const double discriminant = sum_ab * sum_ab - sum_a * (sum_abb - sh * sh);
        if (discriminant < 0.0) {
            break;
        }
        t = (sum_ab + sqrt(discriminant)) / sum_a;
    }
    return (float)t;
}


static bool fractional_index(
    const localization_eikonal_model_t *model,
    const localization_point3d_t *point,
    double *fx,
    double *fy,
    double *fz
)
{
    const double tolerance = 1.0e-6;
    *fx = (point->x_m - model->origin.x_m) / model->cell_m;
    *fy = (point->y_m - model->origin.y_m) / model->cell_m;
    *fz = model->nz > 1U ? (point->z_m - model->origin.z_m) / model->cell_m : 0.0;
    return
        *fx >= -tolerance && *fx <= (double)(model->nx - 1U) + tolerance &&
        *fy >= -tolerance && *fy <= (double)(model->ny - 1U) + tolerance &&
        *fz >= -tolerance && *fz <= (double)(model->nz - 1U) + tolerance;
}


/* Straight-ray time from fractional index (fx, fy, fz) to a node: the
 * length times the mean slowness sampled along the segment (nearest node,
 * quarter-cell steps). INFINITY when the segment touches an impassable
 * node -- the marching then finds the way around. Samples within one cell
 * of the source are not tested: a sensor on a surface sits next to void
 * (air) by definition, and its own nearest node may be one. */
static double line_of_sight_time(
    const localization_eikonal_model_t *model,
    double fx,
    double fy,
    double fz,
    size_t ix,
    size_t iy,
    size_t iz,
    double distance_cells
)
{
    const size_t node = ix + model->nx * (iy + model->ny * iz);
    if (distance_cells <= 1.0) {
        return distance_cells * model->cell_m * (double)model->slowness_s_per_m[node];
    }
    const size_t steps = (size_t)ceil(distance_cells * 4.0) + 1U;
    double sum = 0.0;
    size_t samples = 0U;
    for (size_t k = 0U; k <= steps; ++k) {
        const double a = (double)k / (double)steps;
        if (a * distance_cells < 1.0) {
            continue;
        }
        const long jx = lround(fx + a * ((double)ix - fx));
        const long jy = lround(fy + a * ((double)iy - fy));
        const long jz = model->nz > 1U ? lround(fz + a * ((double)iz - fz)) : 0;
        if (jx < 0 || jy < 0 || jz < 0 || jx >= (long)model->nx || jy >= (long)model->ny || jz >= (long)model->nz) {
            return INFINITY;
        }
        const float slowness = model->slowness_s_per_m[(size_t)jx + model->nx * ((size_t)jy + model->ny * (size_t)jz)];
        if (!isfinite(slowness)) {
            return INFINITY;
        }
        sum += (double)slowness;
        ++samples;
    }
    return samples > 0U ? distance_cells * model->cell_m * sum / (double)samples : INFINITY;
}


esp_err_t localization_eikonal_travel_times(
    const localization_eikonal_model_t *model,
    const localization_point3d_t *source,
    float *times
)
{
    if (model == NULL || model->slowness_s_per_m == NULL || source == NULL || times == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t count = localization_eikonal_node_count(model);
    for (size_t i = 0U; i < count; ++i) {
        times[i] = INFINITY;
    }
    double fx = 0.0, fy = 0.0, fz = 0.0;
    if (!fractional_index(model, source, &fx, &fy, &fz) || count > (size_t)INT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *state = calloc(count, sizeof(uint8_t));
    int32_t *heap = malloc(count * sizeof(int32_t));
    int32_t *position = malloc(count * sizeof(int32_t));
    if (state == NULL || heap == NULL || position == NULL) {
        free(state);
        free(heap);
        free(position);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0U; i < count; ++i) {
        position[i] = -1;
    }
    node_heap_t h = { heap, position, 0U, times };

    /* Exact straight-ray times in a ball around the source. */
    const double radius = EIKONAL_SOURCE_RADIUS_CELLS;
    const long r = (long)ceil(radius);
    size_t seeded = 0U;
    for (long dz = -r; dz <= r + 1; ++dz) {
        const long iz = (long)floor(fz) + dz;
        if (iz < 0 || iz >= (long)model->nz) {
            continue;
        }
        for (long dy = -r; dy <= r + 1; ++dy) {
            const long iy = (long)floor(fy) + dy;
            if (iy < 0 || iy >= (long)model->ny) {
                continue;
            }
            for (long dx = -r; dx <= r + 1; ++dx) {
                const long ix = (long)floor(fx) + dx;
                if (ix < 0 || ix >= (long)model->nx) {
                    continue;
                }
                const double ddx = (double)ix - fx;
                const double ddy = (double)iy - fy;
                const double ddz = model->nz > 1U ? (double)iz - fz : 0.0;
                const double distance_cells = sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                const size_t node = (size_t)ix + model->nx * ((size_t)iy + model->ny * (size_t)iz);
                if (distance_cells > radius || !isfinite(model->slowness_s_per_m[node])) {
                    continue;
                }
                const double t = line_of_sight_time(
                    model, fx, fy, fz, (size_t)ix, (size_t)iy, (size_t)iz, distance_cells
                );
                if (!isfinite(t)) {
                    continue;
                }
                times[node] = (float)t;
                state[node] = NODE_KNOWN;
                ++seeded;
            }
        }
    }
    if (seeded == 0U) {
        free(state);
        free(heap);
        free(position);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t stride_y = model->nx;
    const size_t stride_z = model->nx * model->ny;
    /* Neighbours of the seeded ball enter the narrow band. */
    for (size_t node = 0U; node < count; ++node) {
        if (state[node] != NODE_KNOWN) {
            continue;
        }
        heap[h.size] = (int32_t)node;
        position[node] = (int32_t)h.size;
        ++h.size;
    }
    /* Seeded nodes are popped like any other (in time order) so their
     * neighbours are solved from them; they are never re-solved. */
    for (size_t slot = h.size / 2U + 1U; slot-- > 0U;) {
        heap_down(&h, slot);
    }

    while (h.size > 0U) {
        const int32_t node = heap_pop(&h);
        state[node] = NODE_KNOWN;
        const size_t ix = (size_t)node % model->nx;
        const size_t iy = ((size_t)node / stride_y) % model->ny;
        const size_t iz = (size_t)node / stride_z;
        const long neighbour_offsets[6][3] = {
            { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
        };
        for (size_t k = 0U; k < 6U; ++k) {
            const long jx = (long)ix + neighbour_offsets[k][0];
            const long jy = (long)iy + neighbour_offsets[k][1];
            const long jz = (long)iz + neighbour_offsets[k][2];
            if (
                jx < 0 || jy < 0 || jz < 0 ||
                jx >= (long)model->nx || jy >= (long)model->ny || jz >= (long)model->nz
            ) {
                continue;
            }
            const size_t neighbour = (size_t)jx + stride_y * (size_t)jy + stride_z * (size_t)jz;
            if (state[neighbour] == NODE_KNOWN || !isfinite(model->slowness_s_per_m[neighbour])) {
                continue;
            }
            const float value = solve_node(model, times, state, (size_t)jx, (size_t)jy, (size_t)jz);
            if (value < times[neighbour]) {
                times[neighbour] = value;
                state[neighbour] = NODE_TRIAL;
                heap_push_or_decrease(&h, (int32_t)neighbour);
            }
        }
    }

    free(state);
    free(heap);
    free(position);
    return ESP_OK;
}


double localization_eikonal_time_at(
    const localization_eikonal_model_t *model,
    const float *times,
    const localization_point3d_t *point
)
{
    double fx = 0.0, fy = 0.0, fz = 0.0;
    if (model == NULL || times == NULL || point == NULL || !fractional_index(model, point, &fx, &fy, &fz)) {
        return INFINITY;
    }
    size_t base[3];
    double frac[3];
    const double f[3] = { fx, fy, fz };
    const size_t counts[3] = { model->nx, model->ny, model->nz };
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (counts[axis] < 2U) {
            base[axis] = 0U;
            frac[axis] = 0.0;
            continue;
        }
        double b = floor(f[axis]);
        if (b < 0.0) {
            b = 0.0;
        }
        if (b > (double)(counts[axis] - 2U)) {
            b = (double)(counts[axis] - 2U);
        }
        base[axis] = (size_t)b;
        frac[axis] = f[axis] - b;
        frac[axis] = frac[axis] < 0.0 ? 0.0 : (frac[axis] > 1.0 ? 1.0 : frac[axis]);
    }

    double interpolated = 0.0;
    bool all_finite = true;
    double best = INFINITY;
    for (size_t corner = 0U; corner < 8U; ++corner) {
        const size_t cx = (corner & 1U) != 0U ? 1U : 0U;
        const size_t cy = (corner & 2U) != 0U ? 1U : 0U;
        const size_t cz = (corner & 4U) != 0U ? 1U : 0U;
        if ((cx && counts[0] < 2U) || (cy && counts[1] < 2U) || (cz && counts[2] < 2U)) {
            continue;
        }
        const size_t node =
            (base[0] + cx) + model->nx * ((base[1] + cy) + model->ny * (base[2] + cz));
        const double weight =
            (cx ? frac[0] : 1.0 - frac[0]) *
            (cy ? frac[1] : 1.0 - frac[1]) *
            (cz ? frac[2] : 1.0 - frac[2]);
        const double t = times[node];
        if (!isfinite(t)) {
            if (weight > 0.0) {
                all_finite = false;
            }
            continue;
        }
        interpolated += weight * t;
        const localization_point3d_t corner_point = localization_eikonal_node_position(model, node);
        const double hop = t + ext_distance(*point, corner_point) * (double)model->slowness_s_per_m[node];
        best = fmin(best, hop);
    }
    return all_finite ? interpolated : best;
}


/* ---- Fields --------------------------------------------------------- */

esp_err_t localization_eikonal_fields_compute(
    const localization_eikonal_model_t *model,
    const localization_point3d_t *sensors,
    size_t sensor_count,
    localization_eikonal_fields_t *fields
)
{
    if (
        model == NULL || model->slowness_s_per_m == NULL || sensors == NULL || fields == NULL ||
        sensor_count == 0U || sensor_count > LOCALIZATION_EXTENDED_MAX_SENSORS
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(fields, 0, sizeof(*fields));
    const size_t node_count = localization_eikonal_node_count(model);
    fields->block = malloc(sensor_count * node_count * sizeof(float));
    if (fields->block == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fields->count = sensor_count;
    fields->node_count = node_count;
    for (size_t i = 0U; i < sensor_count; ++i) {
        fields->fields[i] = fields->block + i * node_count;
        const esp_err_t err = localization_eikonal_travel_times(model, &sensors[i], fields->fields[i]);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            localization_eikonal_fields_free(fields);
            return err;
        }
    }
    return ESP_OK;
}


void localization_eikonal_fields_free(localization_eikonal_fields_t *fields)
{
    if (fields != NULL) {
        free(fields->block);
        memset(fields, 0, sizeof(*fields));
    }
}


bool localization_eikonal_travel_time(
    size_t sensor_index,
    const localization_point3d_t *candidate,
    double *travel_time_s,
    void *context
)
{
    const localization_eikonal_travel_time_context_t *ctx = context;
    if (ctx == NULL || ctx->fields == NULL || sensor_index >= ctx->fields->count || travel_time_s == NULL) {
        return false;
    }
    const double t = localization_eikonal_time_at(ctx->model, ctx->fields->fields[sensor_index], candidate);
    *travel_time_s = t;
    return isfinite(t);
}


/* ---- Location ------------------------------------------------------- */

typedef struct
{
    double arrival_s[LOCALIZATION_EXTENDED_MAX_SENSORS];
    double weight[LOCALIZATION_EXTENDED_MAX_SENSORS];      /* pick weight x robust weight */
    size_t count;
    double sigma_s;
    double scale_sigma;

} locate_problem_t;


/* chi^2 of the (t0, scale) fit at the given per-pick times; INFINITY when a
 * weighted pick is unreachable. */
static double evaluate_times(const locate_problem_t *problem, const double *travel_s, vf_time_fit_t *fit)
{
    double path[LOCALIZATION_EXTENDED_MAX_SENSORS];
    for (size_t i = 0U; i < problem->count; ++i) {
        if (problem->weight[i] > 0.0 && !isfinite(travel_s[i])) {
            return INFINITY;
        }
        path[i] = problem->weight[i] > 0.0 ? travel_s[i] : 0.0;
    }
    if (!vf_fit_travel_times(
            problem->arrival_s, path, problem->weight, problem->count, problem->sigma_s,
            1.0, problem->scale_sigma, fit
        )) {
        return INFINITY;
    }
    return fit->chi_square;
}


/* Best admitted candidate for the current weights: every passable node
 * (exact node times), then a finer lattice of interpolated times around
 * the best node. Returns its chi^2 (INFINITY when none is reachable). */
static double search(
    const localization_eikonal_model_t *model,
    const localization_eikonal_fields_t *fields,
    const locate_problem_t *problem,
    const localization_eikonal_locate_config_t *config,
    localization_point3d_t *best_position,
    vf_time_fit_t *best_fit,
    size_t *evaluated
)
{
    const size_t node_count = localization_eikonal_node_count(model);
    double best_chi = INFINITY;
    for (size_t node = 0U; node < node_count; ++node) {
        if (!isfinite(model->slowness_s_per_m[node])) {
            continue;
        }
        const localization_point3d_t point = localization_eikonal_node_position(model, node);
        if (!localization_candidate_admitted(&config->candidate_filter, &point)) {
            continue;
        }
        double travel[LOCALIZATION_EXTENDED_MAX_SENSORS];
        for (size_t i = 0U; i < problem->count; ++i) {
            travel[i] = fields->fields[i][node];
        }
        vf_time_fit_t fit;
        const double chi = evaluate_times(problem, travel, &fit);
        ++*evaluated;
        if (chi < best_chi) {
            best_chi = chi;
            *best_fit = fit;
            *best_position = point;
        }
    }
    if (!isfinite(best_chi)) {
        return INFINITY;
    }

    const localization_point3d_t centre = *best_position;
    const double step = model->cell_m / (double)config->refine_divisor;
    const long span = (long)config->refine_divisor;
    const long span_z = model->nz > 1U ? span : 0;
    for (long dz = -span_z; dz <= span_z; ++dz) {
        for (long dy = -span; dy <= span; ++dy) {
            for (long dx = -span; dx <= span; ++dx) {
                const localization_point3d_t point = {
                    centre.x_m + (double)dx * step,
                    centre.y_m + (double)dy * step,
                    centre.z_m + (double)dz * step,
                };
                if (!localization_candidate_admitted(&config->candidate_filter, &point)) {
                    continue;
                }
                double travel[LOCALIZATION_EXTENDED_MAX_SENSORS];
                for (size_t i = 0U; i < problem->count; ++i) {
                    travel[i] = localization_eikonal_time_at(model, fields->fields[i], &point);
                }
                vf_time_fit_t fit;
                const double chi = evaluate_times(problem, travel, &fit);
                ++*evaluated;
                if (chi < best_chi) {
                    best_chi = chi;
                    *best_fit = fit;
                    *best_position = point;
                }
            }
        }
    }
    return best_chi;
}


/* Upper end of a plausible chi^2 for `dof` degrees of freedom (mean + 3
 * standard deviations). */
static double chi_square_limit(int dof)
{
    return dof > 0 ? (double)dof + 3.0 * sqrt(2.0 * (double)dof) : 0.0;
}


static double pick_residual(
    const localization_eikonal_model_t *model,
    const localization_eikonal_fields_t *fields,
    const locate_problem_t *problem,
    size_t i,
    const localization_point3d_t *position,
    const vf_time_fit_t *fit
)
{
    const double travel = localization_eikonal_time_at(model, fields->fields[i], position);
    return problem->arrival_s[i] - fit->t0_s - fit->slowness_s_per_m * travel;
}


esp_err_t localization_eikonal_locate(
    const localization_eikonal_model_t *model,
    const localization_eikonal_fields_t *fields,
    const localization_eikonal_pick_t *picks,
    size_t pick_count,
    const localization_eikonal_locate_config_t *config,
    localization_eikonal_result_t *result
)
{
    if (
        model == NULL || model->slowness_s_per_m == NULL || fields == NULL || picks == NULL ||
        config == NULL || result == NULL || pick_count == 0U || pick_count != fields->count ||
        fields->node_count != localization_eikonal_node_count(model) ||
        !(config->timing_uncertainty_us > 0.0) || !isfinite(config->timing_uncertainty_us) ||
        !isfinite(config->velocity_scale_sigma) || config->refine_divisor < 2U
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->cell_m = model->cell_m;
    result->resolution_limited = model->resolution_limited;

    locate_problem_t problem;
    memset(&problem, 0, sizeof(problem));
    problem.count = pick_count;
    problem.sigma_s = config->timing_uncertainty_us * 1.0e-6;
    problem.scale_sigma = config->velocity_scale_sigma;
    int64_t earliest = INT64_MAX;
    for (size_t i = 0U; i < pick_count; ++i) {
        if (!(picks[i].weight >= 0.0) || !isfinite(picks[i].weight)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (picks[i].weight > 0.0 && picks[i].arrival_time_us < earliest) {
            earliest = picks[i].arrival_time_us;
        }
    }

    const size_t node_count = localization_eikonal_node_count(model);
    /* A pick whose sensor reaches no admitted node is not used. */
    uint32_t reachable = 0U;
    size_t admitted_count = 0U;
    for (size_t node = 0U; node < node_count; ++node) {
        const localization_point3d_t point = localization_eikonal_node_position(model, node);
        if (!isfinite(model->slowness_s_per_m[node]) || !localization_candidate_admitted(&config->candidate_filter, &point)) {
            continue;
        }
        ++admitted_count;
        for (size_t i = 0U; i < pick_count; ++i) {
            if (isfinite(fields->fields[i][node])) {
                reachable |= 1U << i;
            }
        }
    }
    const size_t spatial = model->nz > 1U ? 3U : 2U;
    const size_t unknowns = spatial + 1U + (config->velocity_scale_sigma < 0.0 ? 1U : 0U);
    for (size_t i = 0U; i < pick_count; ++i) {
        if (picks[i].weight > 0.0) {
            if ((reachable & (1U << i)) != 0U) {
                problem.weight[i] = picks[i].weight;
                problem.arrival_s[i] = (double)(picks[i].arrival_time_us - earliest) * 1.0e-6;
                result->pick_mask_used |= 1U << i;
                ++result->picks_used;
            } else {
                result->pick_mask_unreachable |= 1U << i;
            }
        }
    }
    if (admitted_count == 0U) {
        result->status = LOCALIZATION_EIKONAL_NO_ALLOWED_CANDIDATE;
        return ESP_OK;
    }
    if (result->picks_used < unknowns) {
        result->status = LOCALIZATION_EIKONAL_INSUFFICIENT_PICKS;
        return ESP_OK;
    }

    localization_point3d_t best_position = model->origin;
    vf_time_fit_t best_fit;
    memset(&best_fit, 0, sizeof(best_fit));
    double best_chi = search(model, fields, &problem, config, &best_position, &best_fit, &result->candidates_evaluated);
    if (!isfinite(best_chi)) {
        result->status = LOCALIZATION_EIKONAL_NO_REACHABLE_CANDIDATE;
        return ESP_OK;
    }

    /* One bad pick: with two picks to spare, an implausible chi^2 is
     * tested against every leave-one-out subset. A pick is rejected only
     * when dropping it leaves a plausible fit and it disagrees with that
     * fit by more than 3 sigma. */
    const int dof = (int)result->picks_used - (int)unknowns;
    if (dof >= 2 && best_chi > chi_square_limit(dof)) {
        double subset_best = INFINITY;
        size_t dropped = SIZE_MAX;
        localization_point3d_t subset_position = best_position;
        vf_time_fit_t subset_fit = best_fit;
        for (size_t j = 0U; j < pick_count; ++j) {
            if ((result->pick_mask_used & (1U << j)) == 0U) {
                continue;
            }
            locate_problem_t trial = problem;
            trial.weight[j] = 0.0;
            localization_point3d_t position = best_position;
            vf_time_fit_t fit;
            const double chi = search(model, fields, &trial, config, &position, &fit, &result->candidates_evaluated);
            if (chi < subset_best) {
                subset_best = chi;
                dropped = j;
                subset_position = position;
                subset_fit = fit;
            }
        }
        if (
            dropped != SIZE_MAX && subset_best <= chi_square_limit(dof - 1) &&
            fabs(pick_residual(model, fields, &problem, dropped, &subset_position, &subset_fit)) >
                3.0 * problem.sigma_s
        ) {
            problem.weight[dropped] = 0.0;
            result->pick_mask_rejected |= 1U << dropped;
            best_position = subset_position;
            best_fit = subset_fit;
        }
    }

    /* Robust (Huber by default) reweighting of the remaining picks. */
    const localization_extended_loss_t loss = config->loss;
    const double k = config->tuning_k > 0.0
        ? config->tuning_k
        : (loss == LOCALIZATION_EXTENDED_LOSS_TUKEY ? LOCALIZATION_EXTENDED_TUKEY_K : LOCALIZATION_EXTENDED_HUBER_K);
    for (size_t round = 0U; round < EIKONAL_ROBUST_ROUNDS; ++round) {
        locate_problem_t weighted = problem;
        for (size_t i = 0U; i < pick_count; ++i) {
            if (!(problem.weight[i] > 0.0)) {
                continue;
            }
            const double residual = pick_residual(model, fields, &problem, i, &best_position, &best_fit);
            double w = localization_extended_robust_weight(loss, residual / problem.sigma_s, k);
            weighted.weight[i] = problem.weight[i] * (w < 1.0e-6 ? 1.0e-6 : w);
        }
        localization_point3d_t position = best_position;
        vf_time_fit_t fit;
        if (isfinite(search(model, fields, &weighted, config, &position, &fit, &result->candidates_evaluated))) {
            best_position = position;
            best_fit = fit;
        }
    }

    double sum = 0.0;
    size_t used = 0U;
    for (size_t i = 0U; i < pick_count; ++i) {
        if (!(problem.weight[i] > 0.0)) {
            continue;
        }
        const double residual = pick_residual(model, fields, &problem, i, &best_position, &best_fit);
        sum += residual * residual;
        ++used;
        if (localization_extended_robust_weight(loss, residual / problem.sigma_s, k) < 0.5) {
            result->pick_mask_downweighted |= 1U << i;
        }
    }
    result->pick_mask_used &= ~result->pick_mask_rejected;
    result->picks_used = used;
    result->status = LOCALIZATION_EIKONAL_SOLVED;
    result->position = best_position;
    result->emission_time_us = earliest + (int64_t)llround(best_fit.t0_s * 1.0e6);
    result->velocity_scale = best_fit.slowness_s_per_m;
    result->rms_residual_us = used > 0U ? sqrt(sum / (double)used) * 1.0e6 : 0.0;
    return ESP_OK;
}


/* ---- Calibration ---------------------------------------------------- */

localization_eikonal_calibration_config_t localization_eikonal_calibration_default_config(void)
{
    localization_eikonal_calibration_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_iterations = 10U;
    config.relaxation = 0.7;
    config.smoothing_passes = 1U;
    return config;
}


/* Per-shot origin time: known, or the mean of (arrival - modelled time). */
static bool shot_origin(
    const localization_eikonal_model_t *model,
    const localization_eikonal_fields_t *fields,
    const localization_eikonal_shot_t *shot,
    size_t sensor_count,
    double *origin_s
)
{
    if (shot->origin_time_known) {
        *origin_s = (double)shot->origin_time_us * 1.0e-6;
        return true;
    }
    double sum = 0.0;
    size_t count = 0U;
    for (size_t s = 0U; s < sensor_count; ++s) {
        if (!shot->arrival_valid[s]) {
            continue;
        }
        const double t = localization_eikonal_time_at(model, fields->fields[s], &shot->position);
        if (isfinite(t)) {
            sum += (double)shot->arrival_time_us[s] * 1.0e-6 - t;
            ++count;
        }
    }
    if (count < 2U) {
        return false;
    }
    *origin_s = sum / (double)count;
    return true;
}


static double calibration_rms(
    const localization_eikonal_model_t *model,
    const localization_eikonal_fields_t *fields,
    const localization_eikonal_shot_t *shots,
    size_t shot_count,
    size_t sensor_count,
    size_t *rays_out
)
{
    double sum = 0.0;
    size_t rays = 0U;
    for (size_t k = 0U; k < shot_count; ++k) {
        double origin = 0.0;
        if (!shot_origin(model, fields, &shots[k], sensor_count, &origin)) {
            continue;
        }
        for (size_t s = 0U; s < sensor_count; ++s) {
            if (!shots[k].arrival_valid[s]) {
                continue;
            }
            const double t = localization_eikonal_time_at(model, fields->fields[s], &shots[k].position);
            if (!isfinite(t)) {
                continue;
            }
            const double r = (double)shots[k].arrival_time_us[s] * 1.0e-6 - origin - t;
            sum += r * r;
            ++rays;
        }
    }
    if (rays_out != NULL) {
        *rays_out = rays;
    }
    return rays > 0U ? sqrt(sum / (double)rays) : INFINITY;
}


/* Gradient of a field at a point by central differences (one-sided where
 * a side is unreachable). */
static bool field_gradient(
    const localization_eikonal_model_t *model,
    const float *times,
    const localization_point3d_t *point,
    double gradient[3]
)
{
    const double d = 0.5 * model->cell_m;
    const size_t axes = model->nz > 1U ? 3U : 2U;
    gradient[2] = 0.0;
    const double centre = localization_eikonal_time_at(model, times, point);
    if (!isfinite(centre)) {
        return false;
    }
    for (size_t axis = 0U; axis < axes; ++axis) {
        localization_point3d_t plus = *point;
        localization_point3d_t minus = *point;
        double *p = axis == 0U ? &plus.x_m : (axis == 1U ? &plus.y_m : &plus.z_m);
        double *m = axis == 0U ? &minus.x_m : (axis == 1U ? &minus.y_m : &minus.z_m);
        *p += d;
        *m -= d;
        const double tp = localization_eikonal_time_at(model, times, &plus);
        const double tm = localization_eikonal_time_at(model, times, &minus);
        if (isfinite(tp) && isfinite(tm)) {
            gradient[axis] = (tp - tm) / (2.0 * d);
        } else if (isfinite(tp)) {
            gradient[axis] = (tp - centre) / d;
        } else if (isfinite(tm)) {
            gradient[axis] = (centre - tm) / d;
        } else {
            gradient[axis] = 0.0;
        }
    }
    const double norm = sqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1] + gradient[2] * gradient[2]);
    return norm > 1.0e-12 && isfinite(norm);
}


static void deposit(
    const localization_eikonal_model_t *model,
    const localization_point3d_t *point,
    double length,
    double value,
    double *numerator,
    double *denominator
)
{
    double fx = 0.0, fy = 0.0, fz = 0.0;
    if (!fractional_index(model, point, &fx, &fy, &fz)) {
        return;
    }
    const double f[3] = { fx, fy, fz };
    const size_t counts[3] = { model->nx, model->ny, model->nz };
    size_t base[3];
    double frac[3];
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (counts[axis] < 2U) {
            base[axis] = 0U;
            frac[axis] = 0.0;
            continue;
        }
        double b = floor(f[axis]);
        b = b < 0.0 ? 0.0 : (b > (double)(counts[axis] - 2U) ? (double)(counts[axis] - 2U) : b);
        base[axis] = (size_t)b;
        frac[axis] = f[axis] - b;
    }
    for (size_t corner = 0U; corner < 8U; ++corner) {
        const size_t cx = (corner & 1U) != 0U ? 1U : 0U;
        const size_t cy = (corner & 2U) != 0U ? 1U : 0U;
        const size_t cz = (corner & 4U) != 0U ? 1U : 0U;
        if ((cx && counts[0] < 2U) || (cy && counts[1] < 2U) || (cz && counts[2] < 2U)) {
            continue;
        }
        const size_t node = (base[0] + cx) + model->nx * ((base[1] + cy) + model->ny * (base[2] + cz));
        if (!isfinite(model->slowness_s_per_m[node])) {
            continue;
        }
        const double weight =
            (cx ? frac[0] : 1.0 - frac[0]) * (cy ? frac[1] : 1.0 - frac[1]) * (cz ? frac[2] : 1.0 - frac[2]);
        numerator[node] += weight * length * value;
        denominator[node] += weight * length;
    }
}


/* Steepest descent of the sensor's field from the shot to the sensor;
 * deposit != false spreads `value` (a slowness correction) along it.
 * Returns the path length, or a negative value when the ray gets lost. */
static double trace_ray(
    const localization_eikonal_model_t *model,
    const float *times,
    const localization_point3d_t *from,
    const localization_point3d_t *to,
    double value,
    double *numerator,
    double *denominator
)
{
    const double step = 0.5 * model->cell_m;
    const size_t max_steps = 4U * (model->nx + model->ny + model->nz);
    localization_point3d_t p = *from;
    if (model->nz == 1U) {
        p.z_m = model->origin.z_m;
    }
    localization_point3d_t target = *to;
    if (model->nz == 1U) {
        target.z_m = model->origin.z_m;
    }
    double length = 0.0;
    for (size_t k = 0U; k < max_steps; ++k) {
        const double remaining = ext_distance(p, target);
        if (remaining <= step) {
            if (numerator != NULL) {
                const localization_point3d_t mid = {
                    0.5 * (p.x_m + target.x_m), 0.5 * (p.y_m + target.y_m), 0.5 * (p.z_m + target.z_m),
                };
                deposit(model, &mid, remaining, value, numerator, denominator);
            }
            return length + remaining;
        }
        double g[3];
        if (!field_gradient(model, times, &p, g)) {
            return -1.0;
        }
        const double norm = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        const localization_point3d_t next = {
            p.x_m - step * g[0] / norm, p.y_m - step * g[1] / norm, p.z_m - step * g[2] / norm,
        };
        if (numerator != NULL) {
            const localization_point3d_t mid = {
                0.5 * (p.x_m + next.x_m), 0.5 * (p.y_m + next.y_m), 0.5 * (p.z_m + next.z_m),
            };
            deposit(model, &mid, step, value, numerator, denominator);
        }
        length += step;
        p = next;
    }
    return -1.0;
}


esp_err_t localization_eikonal_calibrate(
    localization_eikonal_model_t *model,
    const localization_point3d_t *sensors,
    size_t sensor_count,
    const localization_eikonal_shot_t *shots,
    size_t shot_count,
    const localization_eikonal_calibration_config_t *config,
    localization_eikonal_calibration_result_t *result
)
{
    if (
        model == NULL || model->slowness_s_per_m == NULL || sensors == NULL || shots == NULL ||
        config == NULL || result == NULL ||
        sensor_count == 0U || sensor_count > LOCALIZATION_EXTENDED_MAX_SENSORS || shot_count == 0U ||
        !(config->timing_uncertainty_us > 0.0) ||
        !(config->min_velocity_mps > 0.0) || !(config->max_velocity_mps > config->min_velocity_mps) ||
        !isfinite(config->max_velocity_mps) ||
        !(config->relaxation > 0.0) || config->relaxation > 1.0 || config->max_iterations == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    const size_t node_count = localization_eikonal_node_count(model);
    localization_eikonal_fields_t fields;
    esp_err_t err = localization_eikonal_fields_compute(model, sensors, sensor_count, &fields);
    if (err != ESP_OK) {
        return err;
    }
    double *numerator = malloc(node_count * sizeof(double));
    double *denominator = malloc(node_count * sizeof(double));
    float *backup = malloc(node_count * sizeof(float));
    float *initial = malloc(node_count * sizeof(float));
    if (numerator == NULL || denominator == NULL || backup == NULL || initial == NULL) {
        free(numerator);
        free(denominator);
        free(backup);
        free(initial);
        localization_eikonal_fields_free(&fields);
        return ESP_ERR_NO_MEM;
    }
    memcpy(initial, model->slowness_s_per_m, node_count * sizeof(float));

    size_t rays = 0U;
    double rms = calibration_rms(model, &fields, shots, shot_count, sensor_count, &rays);
    result->rays_used = rays;
    result->rms_before_us = isfinite(rms) ? rms * 1.0e6 : 0.0;
    if (rays < 2U) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    const float min_slowness = (float)(1.0 / config->max_velocity_mps);
    const float max_slowness = (float)(1.0 / config->min_velocity_mps);
    double relaxation = config->relaxation;
    for (size_t iteration = 0U; iteration < config->max_iterations && relaxation > 0.05; ++iteration) {
        memset(numerator, 0, node_count * sizeof(double));
        memset(denominator, 0, node_count * sizeof(double));
        for (size_t k = 0U; k < shot_count; ++k) {
            double origin = 0.0;
            if (!shot_origin(model, &fields, &shots[k], sensor_count, &origin)) {
                continue;
            }
            for (size_t s = 0U; s < sensor_count; ++s) {
                if (!shots[k].arrival_valid[s]) {
                    continue;
                }
                const double t = localization_eikonal_time_at(model, fields.fields[s], &shots[k].position);
                if (!isfinite(t)) {
                    continue;
                }
                const double residual = (double)shots[k].arrival_time_us[s] * 1.0e-6 - origin - t;
                const double length = trace_ray(model, fields.fields[s], &shots[k].position, &sensors[s], 0.0, NULL, NULL);
                if (!(length > 0.0)) {
                    continue;
                }
                /* A uniform slowness change d along the ray changes its
                 * time by d x length. */
                (void)trace_ray(model, fields.fields[s], &shots[k].position, &sensors[s],
                                residual / length, numerator, denominator);
            }
        }

        /* Per-node mean correction, then coverage-weighted smoothing. */
        for (size_t node = 0U; node < node_count; ++node) {
            numerator[node] = denominator[node] > 0.0 ? numerator[node] / denominator[node] : 0.0;
        }
        for (size_t pass = 0U; pass < config->smoothing_passes; ++pass) {
            double *smoothed = malloc(node_count * sizeof(double));
            if (smoothed == NULL) {
                err = ESP_ERR_NO_MEM;
                goto done;
            }
            for (size_t node = 0U; node < node_count; ++node) {
                smoothed[node] = numerator[node];
                if (!(denominator[node] > 0.0)) {
                    continue;
                }
                const size_t ix = node % model->nx;
                const size_t iy = (node / model->nx) % model->ny;
                const size_t iz = node / (model->nx * model->ny);
                double sum = denominator[node] * numerator[node];
                double weight = denominator[node];
                const long offsets[6][3] = {
                    { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
                };
                for (size_t n = 0U; n < 6U; ++n) {
                    const long jx = (long)ix + offsets[n][0];
                    const long jy = (long)iy + offsets[n][1];
                    const long jz = (long)iz + offsets[n][2];
                    if (jx < 0 || jy < 0 || jz < 0 ||
                        jx >= (long)model->nx || jy >= (long)model->ny || jz >= (long)model->nz) {
                        continue;
                    }
                    const size_t neighbour = (size_t)jx + model->nx * ((size_t)jy + model->ny * (size_t)jz);
                    if (denominator[neighbour] > 0.0) {
                        sum += denominator[neighbour] * numerator[neighbour];
                        weight += denominator[neighbour];
                    }
                }
                smoothed[node] = sum / weight;
            }
            memcpy(numerator, smoothed, node_count * sizeof(double));
            free(smoothed);
        }

        memcpy(backup, model->slowness_s_per_m, node_count * sizeof(float));
        for (size_t node = 0U; node < node_count; ++node) {
            if (!(denominator[node] > 0.0) || !isfinite(model->slowness_s_per_m[node])) {
                continue;
            }
            float updated = (float)((double)model->slowness_s_per_m[node] + relaxation * numerator[node]);
            updated = updated < min_slowness ? min_slowness : (updated > max_slowness ? max_slowness : updated);
            model->slowness_s_per_m[node] = updated;
        }

        localization_eikonal_fields_t trial;
        err = localization_eikonal_fields_compute(model, sensors, sensor_count, &trial);
        if (err != ESP_OK) {
            memcpy(model->slowness_s_per_m, backup, node_count * sizeof(float));
            goto done;
        }
        const double trial_rms = calibration_rms(model, &trial, shots, shot_count, sensor_count, NULL);
        if (trial_rms < rms * (1.0 - 1.0e-4)) {
            localization_eikonal_fields_free(&fields);
            fields = trial;
            rms = trial_rms;
            ++result->iterations;
        } else {
            localization_eikonal_fields_free(&trial);
            memcpy(model->slowness_s_per_m, backup, node_count * sizeof(float));
            relaxation *= 0.5;
        }
    }
    err = ESP_OK;

done:
    result->rms_after_us = isfinite(rms) ? rms * 1.0e6 : 0.0;
    result->min_velocity_mps = 0.0;
    result->max_velocity_mps = 0.0;
    for (size_t node = 0U; node < node_count; ++node) {
        if (model->slowness_s_per_m[node] != initial[node] && isfinite(model->slowness_s_per_m[node])) {
            const double v = 1.0 / (double)model->slowness_s_per_m[node];
            if (result->nodes_updated == 0U) {
                result->min_velocity_mps = v;
                result->max_velocity_mps = v;
            }
            result->min_velocity_mps = fmin(result->min_velocity_mps, v);
            result->max_velocity_mps = fmax(result->max_velocity_mps, v);
            ++result->nodes_updated;
        }
    }
    free(numerator);
    free(denominator);
    free(backup);
    free(initial);
    localization_eikonal_fields_free(&fields);
    return err;
}
