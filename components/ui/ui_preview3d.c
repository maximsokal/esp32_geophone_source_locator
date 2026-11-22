#include "ui_preview3d.h"

#include <float.h>
#include <math.h>
#include <limits.h>
#include <string.h>

#define PREVIEW_PI 3.14159265358979f
#define DEG_TO_RAD (PREVIEW_PI / 180.0f)
#define YAW_STEP_DEG 15.0f
#define PITCH_STEP_DEG 10.0f
#define ISO_YAW_DEG 45.0f
#define ISO_PITCH_DEG 35.26f
#define FIT_FRACTION 0.9f
#define MIN_RADIUS_M 0.5f
#define SENSOR_MARKER_HALF 2
#define MIN_SOURCE_PIXELS 3.0f

void ui_preview3d_view_preset(ui_preview3d_view_t *view, ui_preview3d_preset_t preset)
{
    if (view == NULL) {
        return;
    }
    view->zoom = 1.0f;
    view->preset = preset;
    switch (preset) {
    case UI_PREVIEW3D_PRESET_TOP:
        view->yaw_deg = 0.0f;
        view->pitch_deg = 90.0f;
        break;
    case UI_PREVIEW3D_PRESET_SIDE:
        view->yaw_deg = 0.0f;
        view->pitch_deg = 0.0f;
        break;
    case UI_PREVIEW3D_PRESET_ISO:
    case UI_PREVIEW3D_PRESET_FREE:
    default:
        view->yaw_deg = ISO_YAW_DEG;
        view->pitch_deg = ISO_PITCH_DEG;
        view->preset = UI_PREVIEW3D_PRESET_ISO;
        break;
    }
}

static float wrap_yaw(float yaw)
{
    while (yaw >= 360.0f) {
        yaw -= 360.0f;
    }
    while (yaw < 0.0f) {
        yaw += 360.0f;
    }
    return yaw;
}

bool ui_preview3d_view_apply_key(ui_preview3d_view_t *view, ui_preview3d_key_t key)
{
    float pitch;

    if (view == NULL) {
        return false;
    }
    switch (key) {
    case UI_PREVIEW3D_KEY_LEFT:
        return false;
    case UI_PREVIEW3D_KEY_RIGHT:
        view->yaw_deg = wrap_yaw(view->yaw_deg + YAW_STEP_DEG);
        view->preset = UI_PREVIEW3D_PRESET_FREE;
        return true;
    case UI_PREVIEW3D_KEY_UP:
    case UI_PREVIEW3D_KEY_DOWN:
        pitch = view->pitch_deg + (key == UI_PREVIEW3D_KEY_UP ? PITCH_STEP_DEG : -PITCH_STEP_DEG);
        pitch = pitch < 0.0f ? 0.0f : (pitch > 90.0f ? 90.0f : pitch);
        if (pitch == view->pitch_deg) {
            return false;
        }
        view->pitch_deg = pitch;
        view->preset = UI_PREVIEW3D_PRESET_FREE;
        return true;
    case UI_PREVIEW3D_KEY_OK:
        ui_preview3d_view_preset(
            view,
            view->preset == UI_PREVIEW3D_PRESET_TOP ? UI_PREVIEW3D_PRESET_SIDE :
            view->preset == UI_PREVIEW3D_PRESET_SIDE ? UI_PREVIEW3D_PRESET_ISO :
            UI_PREVIEW3D_PRESET_TOP);
        return true;
    default:
        return false;
    }
}

static bool source_candidate_valid(const ui_preview3d_source_candidate_t *candidate)
{
    return candidate != NULL && candidate->method_name != NULL && candidate->method_name[0] != '\0' &&
        candidate->solved && isfinite(candidate->confidence_score) && candidate->confidence_score >= 0.0 &&
        candidate->confidence_score <= 1.0 && isfinite(candidate->position.x) &&
        isfinite(candidate->position.y) && isfinite(candidate->position.z);
}

static bool source_candidate_latest(
    const ui_preview3d_source_candidate_t *candidates,
    size_t candidate_count,
    size_t index)
{
    if (candidates[index].method_name == NULL) return false;
    const ui_preview3d_source_candidate_t *candidate = &candidates[index];
    /* ponytail: O(n^2) over the run-store's 128-record cap; sort only if that cap grows. */
    for (size_t other = 0U; other < candidate_count; ++other) {
        if (other == index) continue;
        const ui_preview3d_source_candidate_t *next = &candidates[other];
        if (next->joint_fusion == candidate->joint_fusion && next->method_name != NULL &&
            strcmp(next->method_name, candidate->method_name) == 0 &&
            (next->computed_at_ms > candidate->computed_at_ms ||
             (next->computed_at_ms == candidate->computed_at_ms && other > index))) return false;
    }
    return true;
}

static bool source_candidate_better(
    const ui_preview3d_source_candidate_t *left,
    const ui_preview3d_source_candidate_t *right)
{
    if (right == NULL) return true;
    if (left->confidence_score != right->confidence_score) return left->confidence_score > right->confidence_score;
    if (left->computed_at_ms != right->computed_at_ms) return left->computed_at_ms > right->computed_at_ms;
    return strcmp(left->method_name, right->method_name) < 0;
}

static void copy_source_candidate(ui_preview3d_source_t *source, const ui_preview3d_source_candidate_t *candidate, bool primary)
{
    source->position = candidate->position;
    source->radius_m = 0.0f;
    source->selected = primary;
    source->uncertainty_valid = candidate->uncertainty_valid &&
        isfinite(candidate->horizontal_std_x_m) && candidate->horizontal_std_x_m > 0.0 &&
        isfinite(candidate->horizontal_std_y_m) && candidate->horizontal_std_y_m > 0.0 &&
        candidate->horizontal_std_x_m <= FLT_MAX && candidate->horizontal_std_y_m <= FLT_MAX;
    source->horizontal_std_x_m = source->uncertainty_valid ? (float)candidate->horizontal_std_x_m : 0.0f;
    source->horizontal_std_y_m = source->uncertainty_valid ? (float)candidate->horizontal_std_y_m : 0.0f;
    source->vertical_std_m = source->uncertainty_valid && isfinite(candidate->vertical_std_m) &&
        candidate->vertical_std_m > 0.0 && candidate->vertical_std_m <= FLT_MAX ?
        (float)candidate->vertical_std_m : 0.0f;
    source->geometry_computed_at_ms = candidate->geometry_computed_at_ms;
}

bool ui_preview3d_uncertainty_to_scan_frame(
    const double sensor_to_scan[16],
    double sigma_x_m,
    double sigma_y_m,
    double sigma_z_m,
    double *scan_x_out,
    double *scan_y_out)
{
    if (scan_x_out != NULL) *scan_x_out = 0.0;
    if (scan_y_out != NULL) *scan_y_out = 0.0;
    if (sensor_to_scan == NULL || scan_x_out == NULL || scan_y_out == NULL ||
        !isfinite(sigma_x_m) || !isfinite(sigma_y_m) || !isfinite(sigma_z_m) ||
        sigma_x_m < 0.0 || sigma_y_m < 0.0 || sigma_z_m < 0.0) return false;

    const double sigma[3] = {sigma_x_m, sigma_y_m, sigma_z_m};
    double extent[2];
    for (size_t row = 0U; row < 2U; ++row) {
        double sum = 0.0;
        for (size_t column = 0U; column < 3U; ++column) {
            const double term = sensor_to_scan[row * 4U + column] * sigma[column];
            sum += term * term;
        }
        extent[row] = sqrt(sum);
        if (!isfinite(extent[row]) || !(extent[row] > 0.0)) return false;
    }
    *scan_x_out = extent[0];
    *scan_y_out = extent[1];
    return true;
}

size_t ui_preview3d_select_sources(
    const ui_preview3d_source_candidate_t *candidates,
    size_t candidate_count,
    ui_preview3d_source_t *sources,
    size_t source_capacity)
{
    size_t chosen[UI_PREVIEW3D_MAX_SOURCES];
    size_t count = 0U;
    size_t capacity = source_capacity < UI_PREVIEW3D_MAX_SOURCES ? source_capacity : UI_PREVIEW3D_MAX_SOURCES;
    size_t primary = SIZE_MAX;
    size_t solved_joint = SIZE_MAX;
    size_t best_method = SIZE_MAX;

    if (sources == NULL || capacity == 0U) return 0U;
    memset(sources, 0, capacity * sizeof(*sources));
    if (candidates == NULL || candidate_count == 0U) return 0U;

    for (size_t i = 0U; i < candidate_count; ++i) {
        if (!source_candidate_latest(candidates, candidate_count, i) || !source_candidate_valid(&candidates[i])) continue;
        if (candidates[i].joint_fusion) {
            if (!candidates[i].uncertainty_too_high &&
                (solved_joint == SIZE_MAX || candidates[i].computed_at_ms > candidates[solved_joint].computed_at_ms ||
                 (candidates[i].computed_at_ms == candidates[solved_joint].computed_at_ms &&
                  source_candidate_better(&candidates[i], &candidates[solved_joint])))) {
                solved_joint = i;
            }
        } else if (best_method == SIZE_MAX || source_candidate_better(&candidates[i], &candidates[best_method])) {
            best_method = i;
        }
    }
    primary = solved_joint != SIZE_MAX ? solved_joint : best_method;

    if (primary != SIZE_MAX) {
        chosen[count] = primary;
        copy_source_candidate(&sources[count], &candidates[primary], true);
        ++count;
    }
    while (count < capacity) {
        size_t next = SIZE_MAX;
        for (size_t i = 0U; i < candidate_count; ++i) {
            bool already_chosen = false;
            if (!source_candidate_latest(candidates, candidate_count, i) || !source_candidate_valid(&candidates[i])) continue;
            for (size_t j = 0U; j < count; ++j) already_chosen = already_chosen || chosen[j] == i;
            if (!already_chosen && (next == SIZE_MAX || source_candidate_better(&candidates[i], &candidates[next]))) next = i;
        }
        if (next == SIZE_MAX) break;
        chosen[count] = next;
        copy_source_candidate(&sources[count], &candidates[next], false);
        ++count;
    }
    return count;
}

bool ui_preview3d_source_to_voxel_index(
    const ui_preview3d_voxel_grid_t *grid,
    const ui_preview3d_vec3_t *position,
    uint32_t *index_out)
{
    if (grid == NULL || position == NULL || index_out == NULL ||
        !(grid->voxel_size_m > 0.0f) || !isfinite(grid->voxel_size_m) ||
        !isfinite(grid->origin.x) || !isfinite(grid->origin.y) || !isfinite(grid->origin.z) ||
        !isfinite(position->x) || !isfinite(position->y) || !isfinite(position->z) ||
        grid->dimensions[0] == 0U || grid->dimensions[1] == 0U || grid->dimensions[2] == 0U) return false;

    const uint64_t volume = (uint64_t)grid->dimensions[0] * grid->dimensions[1] * grid->dimensions[2];
    if (volume > (UINT64_C(1) << 32U)) return false;
    const double coordinates[3] = {
        floor(((double)position->x - grid->origin.x) / grid->voxel_size_m),
        floor(((double)position->y - grid->origin.y) / grid->voxel_size_m),
        floor(((double)position->z - grid->origin.z) / grid->voxel_size_m),
    };
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (!isfinite(coordinates[axis]) || coordinates[axis] < 0.0 || coordinates[axis] >= grid->dimensions[axis]) return false;
    }
    const uint64_t index =
        ((uint64_t)(uint32_t)coordinates[2] * grid->dimensions[1] + (uint32_t)coordinates[1]) * grid->dimensions[0] +
        (uint32_t)coordinates[0];
    if (index > UINT32_MAX) return false;
    *index_out = (uint32_t)index;
    return true;
}

size_t ui_preview3d_source_uncertainty_voxels(
    const ui_preview3d_voxel_grid_t *grid,
    const ui_preview3d_vec3_t *position,
    float sigma_x_m,
    float sigma_y_m,
    bool uncertainty_valid,
    uint32_t *indices,
    size_t capacity)
{
    uint32_t source_index = 0U;
    if (capacity == 0U || indices == NULL ||
        !ui_preview3d_source_to_voxel_index(grid, position, &source_index)) return 0U;
    if (!uncertainty_valid || !(sigma_x_m > 0.0f) || !(sigma_y_m > 0.0f) ||
        !isfinite(sigma_x_m) || !isfinite(sigma_y_m)) {
        indices[0] = source_index;
        return 1U;
    }

    const uint32_t source_x = source_index % grid->dimensions[0];
    const uint32_t source_y = (source_index / grid->dimensions[0]) % grid->dimensions[1];
    const double source_xy[2] = {position->x, position->y};
    const double origin_xy[2] = {grid->origin.x, grid->origin.y};
    const double sigma_xy[2] = {sigma_x_m, sigma_y_m};
    const uint32_t source_cell_xy[2] = {source_x, source_y};
    const uint32_t dimensions_xy[2] = {grid->dimensions[0], grid->dimensions[1]};
    uint32_t first[2], last[2];
    for (size_t axis = 0U; axis < 2U; ++axis) {
        const double minimum = ceil((source_xy[axis] - sigma_xy[axis] - origin_xy[axis]) /
                                    grid->voxel_size_m - 0.5);
        const double maximum = floor((source_xy[axis] + sigma_xy[axis] - origin_xy[axis]) /
                                     grid->voxel_size_m - 0.5);
        if (maximum < 0.0 || minimum >= dimensions_xy[axis]) return 0U;
        first[axis] = minimum <= 0.0 ? 0U : (uint32_t)minimum;
        last[axis] = maximum >= dimensions_xy[axis] - 1U ? dimensions_xy[axis] - 1U : (uint32_t)maximum;
        if (first[axis] > last[axis]) return 0U;
    }

    const double source_cell_center_x = origin_xy[0] + (source_x + 0.5) * grid->voxel_size_m;
    const double source_cell_center_y = origin_xy[1] + (source_y + 0.5) * grid->voxel_size_m;
    const double source_dx = (source_cell_center_x - position->x) / sigma_x_m;
    const double source_dy = (source_cell_center_y - position->y) / sigma_y_m;
    if (source_dx * source_dx + source_dy * source_dy > 1.0) return 0U;

    uint32_t max_radius = source_cell_xy[0] - first[0];
    if (last[0] - source_cell_xy[0] > max_radius) max_radius = last[0] - source_cell_xy[0];
    if (source_cell_xy[1] - first[1] > max_radius) max_radius = source_cell_xy[1] - first[1];
    if (last[1] - source_cell_xy[1] > max_radius) max_radius = last[1] - source_cell_xy[1];

    size_t count = 0U;
    for (uint32_t radius = 0U; radius <= max_radius && count < capacity; ++radius) {
        const uint32_t x_min = source_x > radius ? source_x - radius : 0U;
        const uint32_t x_start = first[0] > x_min ? first[0] : x_min;
        const uint32_t x_max = source_x + radius;
        const uint32_t x_end = last[0] < x_max ? last[0] : x_max;
        const uint32_t y_min = source_y > radius ? source_y - radius : 0U;
        const uint32_t y_start = first[1] > y_min ? first[1] : y_min;
        const uint32_t y_max = source_y + radius;
        const uint32_t y_end = last[1] < y_max ? last[1] : y_max;
        for (uint32_t y = y_start; y <= y_end && count < capacity; ++y) {
            for (uint32_t x = x_start; x <= x_end && count < capacity; ++x) {
                const uint32_t dx_cells = x > source_x ? x - source_x : source_x - x;
                const uint32_t dy_cells = y > source_y ? y - source_y : source_y - y;
                if ((dx_cells > dy_cells ? dx_cells : dy_cells) != radius) continue;
                const double center_x = origin_xy[0] + (x + 0.5) * grid->voxel_size_m;
                const double center_y = origin_xy[1] + (y + 0.5) * grid->voxel_size_m;
                const double dx = (center_x - position->x) / sigma_x_m;
                const double dy = (center_y - position->y) / sigma_y_m;
                if (dx * dx + dy * dy <= 1.0) {
                    const uint64_t index = ((uint64_t)(source_index / ((uint32_t)grid->dimensions[0] * grid->dimensions[1])) *
                                            grid->dimensions[1] + y) * grid->dimensions[0] + x;
                    if (index <= UINT32_MAX) indices[count++] = (uint32_t)index;
                }
            }
        }
        if (radius == UINT32_MAX) break;
    }
    return count;
}

void ui_preview3d_scene_set_occupancy(
    ui_preview3d_scene_t *scene,
    const scan_occupancy_t *occupancy,
    uint64_t geometry_computed_at_ms)
{
    size_t stride;
    size_t out = 0;

    if (scene == NULL) {
        return;
    }
    scene->point_count = 0;
    scene->points_available = 0;
    scene->registered = false;
    scene->registration_geometry_computed_at_ms = 0U;
    scene->voxel_indices = NULL;
    scene->voxel_face_masks = NULL;
    scene->voxel_count = 0U;
    if (occupancy == NULL || occupancy->occupied_keys == NULL || !(occupancy->voxel_size_m > 0.0)) {
        return;
    }
    scene->points_available = occupancy->occupied_count;
    stride = (occupancy->occupied_count + UI_PREVIEW3D_MAX_POINTS - 1U) / UI_PREVIEW3D_MAX_POINTS;
    if (stride == 0U) {
        stride = 1U;
    }
    for (size_t i = 0; i < occupancy->occupied_count && out < UI_PREVIEW3D_MAX_POINTS; i += stride) {
        /* Key layout: see scan_occupancy.c pack_key (16-bit offset indices). */
        const int64_t key = occupancy->occupied_keys[i];
        const int32_t ix = (int32_t)((key >> 32) & 0xFFFF) - 32768;
        const int32_t iy = (int32_t)((key >> 16) & 0xFFFF) - 32768;
        const int32_t iz = (int32_t)(key & 0xFFFF) - 32768;
        scene->points[out].x = (float)(((double)ix + 0.5) * occupancy->voxel_size_m);
        scene->points[out].y = (float)(((double)iy + 0.5) * occupancy->voxel_size_m);
        scene->points[out].z = (float)(((double)iz + 0.5) * occupancy->voxel_size_m);
        out++;
    }
    scene->point_count = out;
    if (scan_occupancy_registration_usable(occupancy, geometry_computed_at_ms)) {
        scene->registered = true;
        scene->registration_geometry_computed_at_ms = occupancy->registration.geometry_computed_at_ms;
        memcpy(scene->sensor_to_scan, occupancy->registration.sensor_to_scan, sizeof(scene->sensor_to_scan));
    }
}

void ui_preview3d_scene_set_voxel_preview(
    ui_preview3d_scene_t *scene,
    const scan_preview_t *preview,
    uint64_t geometry_computed_at_ms,
    uint32_t *draw_order,
    size_t draw_order_capacity)
{
    if (scene == NULL) return;
    scene->point_count = 0U;
    scene->points_available = 0U;
    scene->registered = false;
    scene->registration_geometry_computed_at_ms = 0U;
    scene->voxel_indices = NULL;
    scene->voxel_face_masks = NULL;
    scene->voxel_count = 0U;
    scene->voxel_order = draw_order;
    scene->voxel_order_capacity = draw_order_capacity;
    if (preview == NULL || preview->voxel_count == 0U || preview->indices == NULL || preview->face_masks == NULL) return;

    scene->voxel_indices = preview->indices;
    scene->voxel_face_masks = preview->face_masks;
    scene->voxel_count = preview->voxel_count;
    memcpy(scene->voxel_dimensions, preview->dimensions, sizeof(scene->voxel_dimensions));
    scene->voxel_origin = (ui_preview3d_vec3_t){preview->origin[0], preview->origin[1], preview->origin[2]};
    scene->voxel_size_m = preview->voxel_size_m;
    if (preview->registration_usable && preview->geometry_computed_at_ms == geometry_computed_at_ms) {
        scene->registered = true;
        scene->registration_geometry_computed_at_ms = preview->geometry_computed_at_ms;
        for (size_t i = 0U; i < 16U; ++i) scene->sensor_to_scan[i] = preview->sensor_to_scan[i];
    }
}

void ui_preview3d_project(
    const ui_preview3d_view_t *view,
    const ui_preview3d_vec3_t *centre,
    float scale,
    int width,
    int height,
    const ui_preview3d_vec3_t *point,
    float *out_x,
    float *out_y)
{
    const float yaw = view->yaw_deg * DEG_TO_RAD;
    const float pitch = view->pitch_deg * DEG_TO_RAD;
    const float px = point->x - centre->x;
    const float py = point->y - centre->y;
    const float pz = point->z - centre->z;
    /* Rotate the scene by -yaw about +Z, then tilt the camera by pitch. */
    const float qx = px * cosf(yaw) + py * sinf(yaw);
    const float qy = -px * sinf(yaw) + py * cosf(yaw);
    const float u = qx;
    const float v = pz * cosf(pitch) + qy * sinf(pitch);

    *out_x = (float)width * 0.5f + u * scale;
    *out_y = (float)height * 0.5f - v * scale;
}

bool ui_preview3d_plot(uint8_t *pixels, int width, int height, int x, int y, uint8_t colour)
{
    if (pixels == NULL || x < 0 || y < 0 || x >= width || y >= height) {
        return false;
    }
    pixels[(size_t)y * (size_t)width + (size_t)x] = colour;
    return true;
}

enum { OUT_LEFT = 1, OUT_RIGHT = 2, OUT_TOP = 4, OUT_BOTTOM = 8 };

static int outcode(float x, float y, float max_x, float max_y)
{
    int code = 0;
    if (x < 0.0f) {
        code |= OUT_LEFT;
    } else if (x > max_x) {
        code |= OUT_RIGHT;
    }
    if (y < 0.0f) {
        code |= OUT_TOP;
    } else if (y > max_y) {
        code |= OUT_BOTTOM;
    }
    return code;
}

bool ui_preview3d_draw_line(uint8_t *pixels, int width, int height,
                            float x0, float y0, float x1, float y1, uint8_t colour)
{
    const float max_x = (float)width - 1.0f;
    const float max_y = (float)height - 1.0f;
    int code0;
    int code1;
    int ix0, iy0, ix1, iy1, dx, dy, sx, sy, err;

    if (pixels == NULL || width <= 0 || height <= 0 ||
        !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) {
        return false;
    }
    code0 = outcode(x0, y0, max_x, max_y);
    code1 = outcode(x1, y1, max_x, max_y);
    /* Cohen-Sutherland: clip to [0, width-1] x [0, height-1]. */
    while ((code0 | code1) != 0) {
        const int out = code0 != 0 ? code0 : code1;
        float x, y;
        if ((code0 & code1) != 0) {
            return false;
        }
        if ((out & OUT_BOTTOM) != 0) {
            x = x0 + (x1 - x0) * (max_y - y0) / (y1 - y0);
            y = max_y;
        } else if ((out & OUT_TOP) != 0) {
            x = x0 + (x1 - x0) * (0.0f - y0) / (y1 - y0);
            y = 0.0f;
        } else if ((out & OUT_RIGHT) != 0) {
            y = y0 + (y1 - y0) * (max_x - x0) / (x1 - x0);
            x = max_x;
        } else {
            y = y0 + (y1 - y0) * (0.0f - x0) / (x1 - x0);
            x = 0.0f;
        }
        if (out == code0) {
            x0 = x;
            y0 = y;
            code0 = outcode(x0, y0, max_x, max_y);
        } else {
            x1 = x;
            y1 = y;
            code1 = outcode(x1, y1, max_x, max_y);
        }
    }

    ix0 = (int)lroundf(x0);
    iy0 = (int)lroundf(y0);
    ix1 = (int)lroundf(x1);
    iy1 = (int)lroundf(y1);
    dx = ix1 > ix0 ? ix1 - ix0 : ix0 - ix1;
    dy = iy1 > iy0 ? -(iy1 - iy0) : -(iy0 - iy1);
    sx = ix0 < ix1 ? 1 : -1;
    sy = iy0 < iy1 ? 1 : -1;
    err = dx + dy;
    for (;;) {
        int e2;
        (void)ui_preview3d_plot(pixels, width, height, ix0, iy0, colour);
        if (ix0 == ix1 && iy0 == iy1) {
            break;
        }
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            ix0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            iy0 += sy;
        }
    }
    return true;
}

static bool draw_circle(uint8_t *pixels, int width, int height, float cx, float cy, float r, uint8_t colour)
{
    int x, y, err;
    int icx, icy;
    int ir;

    if (!isfinite(cx) || !isfinite(cy) || !isfinite(r)) {
        return false;
    }
    if (r > (float)(width + height) * 4.0f) {
        r = (float)(width + height) * 4.0f;
    }
    if (cx + r < 0.0f || cy + r < 0.0f || cx - r >= (float)width || cy - r >= (float)height) return false;
    icx = (int)lroundf(cx);
    icy = (int)lroundf(cy);
    ir = (int)lroundf(r);
    if (icx + ir < 0 || icy + ir < 0 || icx - ir >= width || icy - ir >= height) {
        return false;
    }
    /* Midpoint circle; plot() clips each pixel. */
    x = ir;
    y = 0;
    err = 1 - ir;
    while (x >= y) {
        (void)ui_preview3d_plot(pixels, width, height, icx + x, icy + y, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx + y, icy + x, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx - y, icy + x, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx - x, icy + y, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx - x, icy - y, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx - y, icy - x, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx + y, icy - x, colour);
        (void)ui_preview3d_plot(pixels, width, height, icx + x, icy - y, colour);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
    return true;
}

static bool plot_projected_point(uint8_t *pixels, int width, int height, float x, float y, uint8_t colour)
{
    if (!isfinite(x) || !isfinite(y) || x < (float)INT_MIN + 1.0f || x > (float)INT_MAX - 1.0f ||
        y < (float)INT_MIN + 1.0f || y > (float)INT_MAX - 1.0f) return false;
    return ui_preview3d_plot(pixels, width, height, (int)lroundf(x), (int)lroundf(y), colour);
}

static ui_preview3d_vec3_t sensor_to_render(const ui_preview3d_scene_t *scene, const ui_preview3d_vec3_t *p, bool to_scan)
{
    ui_preview3d_vec3_t out = *p;
    if (to_scan) {
        const double *m = scene->sensor_to_scan;
        out.x = (float)(m[0] * p->x + m[1] * p->y + m[2] * p->z + m[3]);
        out.y = (float)(m[4] * p->x + m[5] * p->y + m[6] * p->z + m[7]);
        out.z = (float)(m[8] * p->x + m[9] * p->y + m[10] * p->z + m[11]);
    }
    return out;
}

typedef struct
{
    ui_preview3d_vec3_t min;
    ui_preview3d_vec3_t max;
    bool any;
} bounds_t;

static void bounds_add(bounds_t *b, const ui_preview3d_vec3_t *p, float pad)
{
    if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->z)) {
        return;
    }
    if (!b->any) {
        b->min = *p;
        b->max = *p;
        b->min.x -= pad; b->min.y -= pad; b->min.z -= pad;
        b->max.x += pad; b->max.y += pad; b->max.z += pad;
        b->any = true;
        return;
    }
    b->min.x = fminf(b->min.x, p->x - pad); b->max.x = fmaxf(b->max.x, p->x + pad);
    b->min.y = fminf(b->min.y, p->y - pad); b->max.y = fmaxf(b->max.y, p->y + pad);
    b->min.z = fminf(b->min.z, p->z - pad); b->max.z = fmaxf(b->max.z, p->z + pad);
}

static const float s_face_corners[6][4][3] = {
    {{ 1, -1, -1}, { 1,  1, -1}, { 1,  1,  1}, { 1, -1,  1}},
    {{-1,  1, -1}, {-1, -1, -1}, {-1, -1,  1}, {-1,  1,  1}},
    {{ 1,  1, -1}, {-1,  1, -1}, {-1,  1,  1}, { 1,  1,  1}},
    {{-1, -1, -1}, { 1, -1, -1}, { 1, -1,  1}, {-1, -1,  1}},
    {{-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1}},
    {{-1,  1, -1}, { 1,  1, -1}, { 1, -1, -1}, {-1, -1, -1}},
};

static const uint8_t s_face_colours[6] = {
    UI_PREVIEW3D_PX_VOXEL_SIDE, UI_PREVIEW3D_PX_VOXEL_SIDE,
    UI_PREVIEW3D_PX_VOXEL_FRONT, UI_PREVIEW3D_PX_VOXEL_FRONT,
    UI_PREVIEW3D_PX_VOXEL_TOP, UI_PREVIEW3D_PX_VOXEL_SIDE,
};

void ui_preview3d_voxel_cell(uint32_t index, const uint16_t dimensions[3], uint32_t *x_out, uint32_t *y_out, uint32_t *z_out)
{
    /* The same cell as index % dx, (index / dx) % dy, index / (dx * dy), with two 32-bit divisions: the ESP32-S3 divides 32-bit values in
     * hardware, but 64-bit values in software (about 20 times slower), and the draw-order sort calls this for every comparison. A valid
     * index is below dx * dy * dz <= 2^32, so every quotient fits 32 bits and the result is identical. */
    const uint32_t dx = dimensions[0];
    const uint32_t dy = dimensions[1];
    const uint32_t row = index / dx;
    const uint32_t z = row / dy;
    *x_out = index - row * dx;
    *y_out = row - z * dy;
    *z_out = z;
}

static bool voxel_position(const ui_preview3d_scene_t *scene, uint32_t index, ui_preview3d_vec3_t *position)
{
    uint32_t x;
    uint32_t y;
    uint32_t z;
    ui_preview3d_voxel_cell(index, scene->voxel_dimensions, &x, &y, &z);
    const float half = scene->voxel_size_m * 0.5f;
    position->x = scene->voxel_origin.x + ((float)x + 0.5f) * scene->voxel_size_m;
    position->y = scene->voxel_origin.y + ((float)y + 0.5f) * scene->voxel_size_m;
    position->z = scene->voxel_origin.z + ((float)z + 0.5f) * scene->voxel_size_m;
    return isfinite(position->x) && isfinite(position->y) && isfinite(position->z) && isfinite(half);
}

static bool voxel_scene_valid(const ui_preview3d_scene_t *scene)
{
    if (scene->voxel_indices == NULL || scene->voxel_face_masks == NULL || scene->voxel_count > SCAN_PREVIEW_MAX_VOXELS ||
        !(scene->voxel_size_m > 0.0f) || !isfinite(scene->voxel_size_m) ||
        !isfinite(scene->voxel_origin.x) || !isfinite(scene->voxel_origin.y) || !isfinite(scene->voxel_origin.z)) return false;
    for (size_t axis = 0U; axis < 3U; ++axis) if (scene->voxel_dimensions[axis] == 0U) return false;
    const uint64_t volume = (uint64_t)scene->voxel_dimensions[0] * scene->voxel_dimensions[1] * scene->voxel_dimensions[2];
    if (volume > (UINT64_C(1) << 32U) || scene->voxel_count > volume) return false;
    for (uint32_t i = 0U; i < scene->voxel_count; ++i) {
        const uint8_t mask = scene->voxel_face_masks[i];
        if ((uint64_t)scene->voxel_indices[i] >= volume || mask == 0U || (mask & 0xc0U) != 0U) return false;
    }
    return true;
}

static bool fill_quad(uint8_t *pixels, int width, int height, const float x[4], const float y[4], uint8_t colour, bool *clipped)
{
    double min_y = y[0], max_y = y[0];
    bool any = false;
    *clipped = false;
    for (size_t i = 0U; i < 4U; ++i) {
        if (!isfinite(x[i]) || !isfinite(y[i])) return false;
        min_y = fmin(min_y, y[i]);
        max_y = fmax(max_y, y[i]);
        if (x[i] < 0.0f || x[i] > (float)(width - 1) || y[i] < 0.0f || y[i] > (float)(height - 1)) *clipped = true;
    }
    if (max_y < 0.5 || min_y > (double)height - 0.5) return false;
    const int y_start = (int)ceil(fmin((double)(height - 1), fmax(0.0, min_y - 0.5)));
    const int y_end = (int)floor(fmin((double)(height - 1), max_y - 0.5));
    for (int row = y_start; row <= y_end; ++row) {
        const double scan_y = (double)row + 0.5;
        double left = INFINITY;
        double right = -INFINITY;
        for (size_t edge = 0U; edge < 4U; ++edge) {
            const size_t next = (edge + 1U) & 3U;
            const double y0 = y[edge], y1 = y[next];
            if ((y0 <= scan_y && y1 > scan_y) || (y1 <= scan_y && y0 > scan_y)) {
                const double cross_x = x[edge] + (scan_y - y0) * ((double)x[next] - x[edge]) / (y1 - y0);
                left = fmin(left, cross_x);
                right = fmax(right, cross_x);
            }
        }
        if (left <= right) {
            if (right < 0.5 || left > (double)width - 0.5) continue;
            const int x_start = (int)ceil(fmin((double)(width - 1), fmax(0.0, left - 0.5)));
            const int x_end = (int)floor(fmin((double)(width - 1), right - 0.5));
            for (int column = x_start; column <= x_end; ++column) {
                pixels[(size_t)row * (size_t)width + (size_t)column] = colour;
                any = true;
            }
        }
    }
    return any;
}

static float voxel_depth(uint32_t index, const ui_preview3d_scene_t *scene, const float coefficients[3])
{
    uint32_t cell_x;
    uint32_t cell_y;
    uint32_t cell_z;
    ui_preview3d_voxel_cell(index, scene->voxel_dimensions, &cell_x, &cell_y, &cell_z);
    const float x = (float)cell_x * scene->voxel_size_m;
    const float y = (float)cell_y * scene->voxel_size_m;
    const float z = (float)cell_z * scene->voxel_size_m;
    return x * coefficients[0] + y * coefficients[1] + z * coefficients[2];
}

static bool depth_after(uint32_t left, uint32_t right, const ui_preview3d_scene_t *scene, const float coefficients[3])
{
    const uint32_t left_index = scene->voxel_indices[left];
    const uint32_t right_index = scene->voxel_indices[right];
    const float left_depth = voxel_depth(left_index, scene, coefficients);
    const float right_depth = voxel_depth(right_index, scene, coefficients);
    return left_depth > right_depth || (left_depth == right_depth && left_index > right_index);
}

static void sift_voxel_heap(uint32_t *order, size_t root, size_t count, const ui_preview3d_scene_t *scene, const float coefficients[3])
{
    for (;;) {
        const size_t left = root * 2U + 1U;
        if (left >= count) return;
        size_t largest = left;
        const size_t right = left + 1U;
        if (right < count && depth_after(order[right], order[left], scene, coefficients)) largest = right;
        if (!depth_after(order[largest], order[root], scene, coefficients)) return;
        const uint32_t swap = order[root];
        order[root] = order[largest];
        order[largest] = swap;
        root = largest;
    }
}

static void sort_voxel_order(uint32_t *order, size_t count, const ui_preview3d_scene_t *scene, const ui_preview3d_view_t *view, float coefficients[3])
{
    const float yaw = view->yaw_deg * DEG_TO_RAD;
    const float pitch = view->pitch_deg * DEG_TO_RAD;
    coefficients[0] = -sinf(yaw) * cosf(pitch);
    coefficients[1] = cosf(yaw) * cosf(pitch);
    coefficients[2] = -sinf(pitch);
    for (size_t i = 0U; i < count; ++i) order[i] = (uint32_t)i;
    for (size_t start = count / 2U; start > 0U;) sift_voxel_heap(order, --start, count, scene, coefficients);
    for (size_t end = count; end > 1U;) {
        const uint32_t swap = order[0];
        order[0] = order[--end];
        order[end] = swap;
        sift_voxel_heap(order, 0U, end, scene, coefficients);
    }
}

static bool draw_voxel_face(
    const ui_preview3d_scene_t *scene,
    const ui_preview3d_view_t *view,
    const ui_preview3d_vec3_t *centre,
    float scale,
    int width,
    int height,
    uint8_t *pixels,
    uint32_t index,
    unsigned face,
    uint8_t colour,
    bool *clipped)
{
    ui_preview3d_vec3_t voxel;
    float x[4], y[4];
    if (!voxel_position(scene, index, &voxel)) return false;
    const float half = scene->voxel_size_m * 0.5f;
    for (size_t corner = 0U; corner < 4U; ++corner) {
        const ui_preview3d_vec3_t point = {
            voxel.x + s_face_corners[face][corner][0] * half,
            voxel.y + s_face_corners[face][corner][1] * half,
            voxel.z + s_face_corners[face][corner][2] * half,
        };
        ui_preview3d_project(view, centre, scale, width, height, &point, &x[corner], &y[corner]);
    }
    return fill_quad(pixels, width, height, x, y, colour, clipped);
}

static bool voxel_index_present(const ui_preview3d_scene_t *scene, uint32_t index)
{
    for (uint32_t i = 0U; i < scene->voxel_count; ++i) {
        if (scene->voxel_indices[i] == index) return true;
    }
    return false;
}

static bool voxel_index_in_list(const uint32_t *indices, size_t count, uint32_t index)
{
    for (size_t i = 0U; i < count; ++i) {
        if (indices[i] == index) return true;
    }
    return false;
}

static bool draw_voxel_faces(
    const ui_preview3d_scene_t *scene,
    const ui_preview3d_view_t *view,
    const ui_preview3d_vec3_t *centre,
    float scale,
    int width,
    int height,
    uint8_t *pixels,
    uint32_t index,
    uint8_t faces,
    uint8_t colour,
    const float camera_direction[3],
    ui_preview3d_stats_t *stats)
{
    bool drawn = false;
    for (unsigned face = 0U; face < 6U; ++face) {
        const float normal_dot =
            (face == 0U ? camera_direction[0] : face == 1U ? -camera_direction[0] :
             face == 2U ? camera_direction[1] : face == 3U ? -camera_direction[1] :
             face == 4U ? camera_direction[2] : -camera_direction[2]);
        if ((faces & (1U << face)) == 0U || normal_dot <= 0.0001f) continue;
        bool clipped = false;
        if (draw_voxel_face(scene, view, centre, scale, width, height, pixels, index, face, colour, &clipped)) {
            stats->faces_drawn++;
            drawn = true;
            if (clipped) stats->primitives_clipped++;
        } else {
            stats->primitives_clipped++;
        }
    }
    if (drawn) stats->voxels_drawn++;
    return drawn;
}

static void draw_voxel_scene(
    const ui_preview3d_scene_t *scene,
    const ui_preview3d_view_t *view,
    const ui_preview3d_vec3_t *centre,
    float scale,
    int width,
    int height,
    uint8_t *pixels,
    bool primary_voxel_valid,
    uint32_t primary_voxel_index,
    const uint32_t *uncertainty_voxel_indices,
    size_t uncertainty_voxel_count,
    ui_preview3d_stats_t *stats)
{
    float depth_coefficients[3];
    const float yaw = view->yaw_deg * DEG_TO_RAD;
    const float pitch = view->pitch_deg * DEG_TO_RAD;
    const float camera_direction[3] = {sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch)};
    if (!voxel_scene_valid(scene) || scene->voxel_order == NULL || scene->voxel_order_capacity < scene->voxel_count) {
        stats->empty = true;
        return;
    }
    sort_voxel_order(scene->voxel_order, scene->voxel_count, scene, view, depth_coefficients);
    for (size_t item = scene->voxel_count; item > 0U; --item) {
        const uint32_t source = scene->voxel_order[item - 1U];
        const uint32_t index = scene->voxel_indices[source];
        const uint8_t faces = scene->voxel_face_masks[source];
        const bool primary = primary_voxel_valid && index == primary_voxel_index;
        const bool uncertainty = !primary &&
            voxel_index_in_list(uncertainty_voxel_indices, uncertainty_voxel_count, index);
        bool voxel_drawn = false;
        for (unsigned face = 0U; face < 6U; ++face) {
            const float normal_dot =
                (face == 0U ? camera_direction[0] : face == 1U ? -camera_direction[0] :
                 face == 2U ? camera_direction[1] : face == 3U ? -camera_direction[1] :
                 face == 4U ? camera_direction[2] : -camera_direction[2]);
            if ((faces & (1U << face)) == 0U || normal_dot <= 0.0001f) continue;
            const uint8_t colour = primary ? UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT :
                uncertainty ? UI_PREVIEW3D_PX_VOXEL_UNCERTAINTY : s_face_colours[face];
            bool clipped = false;
            if (draw_voxel_face(scene, view, centre, scale, width, height, pixels,
                                index, face, colour, &clipped)) {
                stats->faces_drawn++;
                voxel_drawn = true;
                if (clipped) stats->primitives_clipped++;
            } else {
                stats->primitives_clipped++;
            }
        }
        if (voxel_drawn) stats->voxels_drawn++;
    }
    for (size_t i = 0U; i < uncertainty_voxel_count; ++i) {
        const uint32_t index = uncertainty_voxel_indices[i];
        if ((primary_voxel_valid && index == primary_voxel_index) || voxel_index_present(scene, index)) continue;
        (void)draw_voxel_faces(scene, view, centre, scale, width, height, pixels, index,
                               0x3fU, UI_PREVIEW3D_PX_VOXEL_UNCERTAINTY, camera_direction, stats);
    }
    if (primary_voxel_valid) {
        if (!voxel_index_present(scene, primary_voxel_index)) {
            (void)draw_voxel_faces(scene, view, centre, scale, width, height, pixels,
                                   primary_voxel_index, 0x3fU, UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT,
                                   camera_direction, stats);
        }
    }
    stats->drew_scan = stats->voxels_drawn > 0U;
}

void ui_preview3d_render(
    const ui_preview3d_scene_t *scene,
    const ui_preview3d_view_t *view,
    ui_preview3d_layer_t layer,
    uint8_t *pixels,
    int width,
    int height,
    ui_preview3d_stats_t *stats)
{
    ui_preview3d_stats_t local = {0};
    ui_preview3d_vec3_t sensors[UI_PREVIEW3D_MAX_SENSORS];
    ui_preview3d_vec3_t sources[UI_PREVIEW3D_MAX_SOURCES];
    uint32_t uncertainty_voxel_indices[UI_PREVIEW3D_SOURCE_UNCERTAINTY_VOXEL_CAP];
    size_t uncertainty_voxel_count = 0U;
    bool source_at_edge[UI_PREVIEW3D_MAX_SOURCES] = {false};
    bool source_stale[UI_PREVIEW3D_MAX_SOURCES] = {false};
    bounds_t bounds = {0};
    bool any_sensor = false;
    bool draw_scan;
    bool draw_sensors;
    bool voxel_mode;
    bool primary_voxel_valid = false;
    uint32_t primary_voxel_index = 0U;
    ui_preview3d_vec3_t centre;
    float radius;
    float scale;

    if (stats == NULL) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));
    if (pixels == NULL || width <= 0 || height <= 0 || width > INT_MAX / 16 || height > INT_MAX / 16 ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        stats->empty = true;
        return;
    }
    memset(pixels, UI_PREVIEW3D_PX_BACKGROUND, (size_t)width * (size_t)height);
    if (scene == NULL || view == NULL) {
        stats->empty = true;
        return;
    }
    voxel_mode = scene->voxel_count > 0U;
    for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
        any_sensor = any_sensor || scene->sensor_valid[i];
    }
    any_sensor = any_sensor || scene->source_count > 0U;

    switch (layer) {
    case UI_PREVIEW3D_LAYER_SCAN:
        draw_scan = voxel_mode || scene->point_count > 0U;
        draw_sensors = draw_scan ? scene->registered && any_sensor : false;
        break;
    case UI_PREVIEW3D_LAYER_SENSORS:
        draw_scan = false;
        draw_sensors = any_sensor;
        break;
    case UI_PREVIEW3D_LAYER_AUTO:
    default:
        draw_scan = voxel_mode || scene->point_count > 0U;
        draw_sensors = any_sensor && (!draw_scan || scene->registered);
        break;
    }
    stats->overlay_suppressed_unregistered = draw_scan && any_sensor && !scene->registered;
    if (draw_scan && scene->source_count > 0U && !scene->registered) {
        stats->source_reason = UI_PREVIEW3D_SOURCE_REASON_UNREGISTERED;
    }

    if (draw_scan && voxel_mode && (!voxel_scene_valid(scene) || scene->voxel_order == NULL || scene->voxel_order_capacity < scene->voxel_count)) {
        stats->empty = true;
        return;
    }

    /* Sensors go to the scan frame only when both layers share the picture. */
    for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
        sensors[i] = sensor_to_render(scene, &scene->sensors[i], draw_scan && draw_sensors);
    }
    for (size_t i = 0; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
        sources[i] = sensor_to_render(scene, &scene->sources[i].position, draw_scan && draw_sensors);
    }
    if (draw_scan && draw_sensors) {
        /* MAX-412: a source solved against another geometry version than the
         * registration would land at a plausible but wrong coordinate. */
        for (size_t i = 0U; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
            if (scene->sources[i].geometry_computed_at_ms != scene->registration_geometry_computed_at_ms) {
                source_stale[i] = true;
                stats->sources_suppressed_stale_geometry++;
            }
        }
    }
    if (draw_scan && draw_sensors && voxel_mode) {
        const ui_preview3d_voxel_grid_t grid = {
            .origin = scene->voxel_origin,
            .dimensions = {scene->voxel_dimensions[0], scene->voxel_dimensions[1], scene->voxel_dimensions[2]},
            .voxel_size_m = scene->voxel_size_m,
        };
        for (size_t i = 0U; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
            uint32_t index = 0U;
            if (source_stale[i]) continue;
            if (ui_preview3d_source_to_voxel_index(&grid, &sources[i], &index)) {
                if (scene->sources[i].selected) {
                    /* The sigmas are sensor-frame axes; rotate their support into the scan frame. */
                    double scan_sigma_x = 0.0, scan_sigma_y = 0.0;
                    const bool sigma_valid = scene->sources[i].uncertainty_valid &&
                        ui_preview3d_uncertainty_to_scan_frame(
                            scene->sensor_to_scan, scene->sources[i].horizontal_std_x_m,
                            scene->sources[i].horizontal_std_y_m, scene->sources[i].vertical_std_m,
                            &scan_sigma_x, &scan_sigma_y);
                    primary_voxel_valid = true;
                    primary_voxel_index = index;
                    uncertainty_voxel_count = ui_preview3d_source_uncertainty_voxels(
                        &grid, &sources[i], (float)scan_sigma_x, (float)scan_sigma_y, sigma_valid,
                        uncertainty_voxel_indices, UI_PREVIEW3D_SOURCE_UNCERTAINTY_VOXEL_CAP);
                }
                continue;
            }

            source_at_edge[i] = true;
            stats->source_reason = UI_PREVIEW3D_SOURCE_REASON_OUTSIDE_GRID;
            const float position[3] = {sources[i].x, sources[i].y, sources[i].z};
            float edge[3];
            for (size_t axis = 0U; axis < 3U; ++axis) {
                const double coordinate = floor(((double)position[axis] -
                    (axis == 0U ? grid.origin.x : axis == 1U ? grid.origin.y : grid.origin.z)) / grid.voxel_size_m);
                const int32_t cell = coordinate < 0.0 ? 0 :
                    coordinate >= grid.dimensions[axis] ? (int32_t)grid.dimensions[axis] - 1 : (int32_t)coordinate;
                const float origin = axis == 0U ? grid.origin.x : axis == 1U ? grid.origin.y : grid.origin.z;
                edge[axis] = origin + ((float)cell + 0.5f) * grid.voxel_size_m;
            }
            sources[i] = (ui_preview3d_vec3_t){edge[0], edge[1], edge[2]};
        }
    }
    if (stats->sources_suppressed_stale_geometry > 0U) {
        stats->source_reason = UI_PREVIEW3D_SOURCE_REASON_STALE_GEOMETRY;
    }

    if (draw_scan) {
        if (voxel_mode) {
            for (uint32_t i = 0U; i < scene->voxel_count; ++i) {
                ui_preview3d_vec3_t voxel;
                if (voxel_position(scene, scene->voxel_indices[i], &voxel)) bounds_add(&bounds, &voxel, scene->voxel_size_m * 0.5f);
            }
        } else {
            for (size_t i = 0; i < scene->point_count; ++i) {
                bounds_add(&bounds, &scene->points[i], 0.0f);
            }
        }
    }
    if (draw_sensors) {
        for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
            if (scene->sensor_valid[i]) {
                bounds_add(&bounds, &sensors[i], 0.0f);
            }
        }
        for (size_t i = 0; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
            if (source_stale[i]) continue;
            bounds_add(&bounds, &sources[i], fmaxf(0.0f, scene->sources[i].radius_m));
        }
    }
    if (voxel_mode && primary_voxel_valid) {
        ui_preview3d_vec3_t voxel;
        if (voxel_position(scene, primary_voxel_index, &voxel)) {
            bounds_add(&bounds, &voxel, scene->voxel_size_m * 0.5f);
        }
        for (size_t i = 0U; i < uncertainty_voxel_count; ++i) {
            if (voxel_position(scene, uncertainty_voxel_indices[i], &voxel)) {
                bounds_add(&bounds, &voxel, scene->voxel_size_m * 0.5f);
            }
        }
    }
    if (!bounds.any) {
        stats->empty = true;
        return;
    }

    /* Fit a bounding sphere so rotating the view never rescales it. */
    centre.x = 0.5f * (bounds.min.x + bounds.max.x);
    centre.y = 0.5f * (bounds.min.y + bounds.max.y);
    centre.z = 0.5f * (bounds.min.z + bounds.max.z);
    radius = 0.5f * hypotf(
        hypotf(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y),
        bounds.max.z - bounds.min.z);
    if (radius < MIN_RADIUS_M) {
        radius = MIN_RADIUS_M;
    }
    scale = (view->zoom > 0.0f ? view->zoom : 1.0f) * FIT_FRACTION * 0.5f *
            (float)(width < height ? width : height) / radius;

    if (draw_scan) {
        if (voxel_mode) {
            draw_voxel_scene(scene, view, &centre, scale, width, height, pixels,
                             primary_voxel_valid, primary_voxel_index,
                             uncertainty_voxel_indices, uncertainty_voxel_count, stats);
        } else {
            for (size_t i = 0; i < scene->point_count; ++i) {
                float x, y;
                ui_preview3d_project(view, &centre, scale, width, height, &scene->points[i], &x, &y);
                if (plot_projected_point(pixels, width, height, x, y, UI_PREVIEW3D_PX_SCAN)) {
                    stats->points_drawn++;
                } else {
                    stats->primitives_clipped++;
                }
            }
            stats->drew_scan = true;
        }
    }
    if (!draw_sensors) {
        return;
    }

    /* Layout skeleton MAIN -> REMOTE, then sources, then markers on top. */
    if (scene->sensor_valid[0]) {
        float mx, my;
        ui_preview3d_project(view, &centre, scale, width, height, &sensors[0], &mx, &my);
        for (size_t i = 1; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
            float x, y;
            if (!scene->sensor_valid[i]) {
                continue;
            }
            ui_preview3d_project(view, &centre, scale, width, height, &sensors[i], &x, &y);
            if (ui_preview3d_draw_line(pixels, width, height, mx, my, x, y, UI_PREVIEW3D_PX_LAYOUT)) {
                stats->lines_drawn++;
            } else {
                stats->primitives_clipped++;
            }
        }
    }
    for (size_t i = 0; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
        float x, y;
        if (source_stale[i]) continue;
        const uint8_t colour = source_at_edge[i] ? UI_PREVIEW3D_PX_SOURCE_OUTSIDE_GRID :
            scene->sources[i].selected ? UI_PREVIEW3D_PX_SOURCE_SELECTED : UI_PREVIEW3D_PX_SOURCE;
        const float r = fmaxf(MIN_SOURCE_PIXELS, scene->sources[i].radius_m * scale);
        ui_preview3d_project(view, &centre, scale, width, height, &sources[i], &x, &y);
        if (draw_circle(pixels, width, height, x, y, r, colour)) {
            (void)ui_preview3d_plot(pixels, width, height, (int)lroundf(x), (int)lroundf(y), colour);
        } else {
            stats->primitives_clipped++;
        }
    }
    for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
        float x, y;
        const uint8_t colour = i == 0U ? UI_PREVIEW3D_PX_MAIN : UI_PREVIEW3D_PX_SENSOR;
        if (!scene->sensor_valid[i]) {
            continue;
        }
        ui_preview3d_project(view, &centre, scale, width, height, &sensors[i], &x, &y);
        if (isfinite(x) && isfinite(y) && x >= (float)INT_MIN + SENSOR_MARKER_HALF && x <= (float)INT_MAX - SENSOR_MARKER_HALF &&
            y >= (float)INT_MIN + SENSOR_MARKER_HALF && y <= (float)INT_MAX - SENSOR_MARKER_HALF) {
            const int marker_x = (int)lroundf(x), marker_y = (int)lroundf(y);
            for (int dy = -SENSOR_MARKER_HALF; dy <= SENSOR_MARKER_HALF; ++dy) {
                for (int dx = -SENSOR_MARKER_HALF; dx <= SENSOR_MARKER_HALF; ++dx) {
                    (void)ui_preview3d_plot(pixels, width, height, marker_x + dx, marker_y + dy, colour);
                }
            }
        }
    }
    stats->drew_sensors = true;
}
