#include "ui_preview3d.h"

#include <math.h>
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
    while (yaw >= 180.0f) {
        yaw -= 360.0f;
    }
    while (yaw < -180.0f) {
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
    case UI_PREVIEW3D_KEY_RIGHT:
        view->yaw_deg = wrap_yaw(view->yaw_deg + (key == UI_PREVIEW3D_KEY_LEFT ? -YAW_STEP_DEG : YAW_STEP_DEG));
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
        memcpy(scene->sensor_to_scan, occupancy->registration.sensor_to_scan, sizeof(scene->sensor_to_scan));
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
    const int icx = (int)lroundf(cx);
    const int icy = (int)lroundf(cy);
    int ir;

    if (!isfinite(cx) || !isfinite(cy) || !isfinite(r)) {
        return false;
    }
    if (r > (float)(width + height) * 4.0f) {
        r = (float)(width + height) * 4.0f;
    }
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
    bounds_t bounds = {0};
    bool any_sensor = false;
    bool draw_scan;
    bool draw_sensors;
    ui_preview3d_vec3_t centre;
    float radius;
    float scale;

    if (stats == NULL) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));
    if (pixels == NULL || width <= 0 || height <= 0) {
        stats->empty = true;
        return;
    }
    memset(pixels, UI_PREVIEW3D_PX_BACKGROUND, (size_t)width * (size_t)height);
    if (scene == NULL || view == NULL) {
        stats->empty = true;
        return;
    }
    for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
        any_sensor = any_sensor || scene->sensor_valid[i];
    }
    any_sensor = any_sensor || scene->source_count > 0U;

    switch (layer) {
    case UI_PREVIEW3D_LAYER_SCAN:
        draw_scan = scene->point_count > 0U;
        draw_sensors = draw_scan ? scene->registered && any_sensor : false;
        break;
    case UI_PREVIEW3D_LAYER_SENSORS:
        draw_scan = false;
        draw_sensors = any_sensor;
        break;
    case UI_PREVIEW3D_LAYER_AUTO:
    default:
        draw_scan = scene->point_count > 0U;
        draw_sensors = any_sensor && (!draw_scan || scene->registered);
        break;
    }
    stats->overlay_suppressed_unregistered = draw_scan && any_sensor && !scene->registered;

    /* Sensors go to the scan frame only when both layers share the picture. */
    for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
        sensors[i] = sensor_to_render(scene, &scene->sensors[i], draw_scan && draw_sensors);
    }
    for (size_t i = 0; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
        sources[i] = sensor_to_render(scene, &scene->sources[i].position, draw_scan && draw_sensors);
    }

    if (draw_scan) {
        for (size_t i = 0; i < scene->point_count; ++i) {
            bounds_add(&bounds, &scene->points[i], 0.0f);
        }
    }
    if (draw_sensors) {
        for (size_t i = 0; i < UI_PREVIEW3D_MAX_SENSORS; ++i) {
            if (scene->sensor_valid[i]) {
                bounds_add(&bounds, &sensors[i], 0.0f);
            }
        }
        for (size_t i = 0; i < scene->source_count && i < UI_PREVIEW3D_MAX_SOURCES; ++i) {
            bounds_add(&bounds, &sources[i], fmaxf(0.0f, scene->sources[i].radius_m));
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
    radius = 0.5f * sqrtf(
        (bounds.max.x - bounds.min.x) * (bounds.max.x - bounds.min.x) +
        (bounds.max.y - bounds.min.y) * (bounds.max.y - bounds.min.y) +
        (bounds.max.z - bounds.min.z) * (bounds.max.z - bounds.min.z));
    if (radius < MIN_RADIUS_M) {
        radius = MIN_RADIUS_M;
    }
    scale = (view->zoom > 0.0f ? view->zoom : 1.0f) * FIT_FRACTION * 0.5f *
            (float)(width < height ? width : height) / radius;

    if (draw_scan) {
        for (size_t i = 0; i < scene->point_count; ++i) {
            float x, y;
            ui_preview3d_project(view, &centre, scale, width, height, &scene->points[i], &x, &y);
            if (ui_preview3d_plot(pixels, width, height, (int)lroundf(x), (int)lroundf(y), UI_PREVIEW3D_PX_SCAN)) {
                stats->points_drawn++;
            } else {
                stats->primitives_clipped++;
            }
        }
        stats->drew_scan = true;
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
        const uint8_t colour = scene->sources[i].selected ? UI_PREVIEW3D_PX_SOURCE_SELECTED : UI_PREVIEW3D_PX_SOURCE;
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
        for (int dy = -SENSOR_MARKER_HALF; dy <= SENSOR_MARKER_HALF; ++dy) {
            for (int dx = -SENSOR_MARKER_HALF; dx <= SENSOR_MARKER_HALF; ++dx) {
                (void)ui_preview3d_plot(pixels, width, height, (int)lroundf(x) + dx, (int)lroundf(y) + dy, colour);
            }
        }
    }
    stats->drew_sensors = true;
}
