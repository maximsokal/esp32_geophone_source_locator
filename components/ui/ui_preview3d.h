#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scan_occupancy.h"

/*
 * MAX-187: bounded software 3D preview of a recording zone for the
 * 320x240 ILI9341. Pure C (no LVGL, no RTOS): orthographic projection,
 * Cohen-Sutherland clipping and Bresenham lines into an 8-bit palette-index
 * buffer that the LVGL screen shows as an indexed canvas.
 *
 * Frames (docs/spatial-frames.md): scan points are in the scan frame,
 * sensors/sources in the sensor frame. They are drawn together only when a
 * usable sensor->scan registration exists (MAX-145); otherwise the caller
 * chooses one layer and the render reports the overlay as suppressed.
 * Both frames are Z-up; heading is unknown, so no compass is drawn.
 */

#define UI_PREVIEW3D_MAX_POINTS 1500U
#define UI_PREVIEW3D_MAX_SENSORS 5U
#define UI_PREVIEW3D_MAX_SOURCES 4U

typedef enum
{
    UI_PREVIEW3D_PX_BACKGROUND = 0,
    UI_PREVIEW3D_PX_SCAN,
    UI_PREVIEW3D_PX_LAYOUT,
    UI_PREVIEW3D_PX_SENSOR,
    UI_PREVIEW3D_PX_MAIN,
    UI_PREVIEW3D_PX_SOURCE,
    UI_PREVIEW3D_PX_SOURCE_SELECTED,
    UI_PREVIEW3D_PX_COUNT
} ui_preview3d_pixel_t;

typedef enum
{
    UI_PREVIEW3D_PRESET_TOP = 0,
    UI_PREVIEW3D_PRESET_SIDE,
    UI_PREVIEW3D_PRESET_ISO,
    UI_PREVIEW3D_PRESET_FREE,
} ui_preview3d_preset_t;

typedef enum
{
    UI_PREVIEW3D_KEY_LEFT = 0,
    UI_PREVIEW3D_KEY_RIGHT,
    UI_PREVIEW3D_KEY_UP,
    UI_PREVIEW3D_KEY_DOWN,
    UI_PREVIEW3D_KEY_OK,
} ui_preview3d_key_t;

typedef struct
{
    /* Yaw about +Z; pitch 0 = side view, 90 = straight down. */
    float yaw_deg;
    float pitch_deg;
    float zoom;
    ui_preview3d_preset_t preset;
} ui_preview3d_view_t;

typedef struct
{
    float x;
    float y;
    float z;
} ui_preview3d_vec3_t;

typedef struct
{
    ui_preview3d_vec3_t position;
    float radius_m;
    bool selected;
} ui_preview3d_source_t;

typedef enum
{
    UI_PREVIEW3D_LAYER_AUTO = 0,   /* combined if registered, else scan */
    UI_PREVIEW3D_LAYER_SCAN,
    UI_PREVIEW3D_LAYER_SENSORS,
} ui_preview3d_layer_t;

typedef struct
{
    /* Scan frame. */
    ui_preview3d_vec3_t points[UI_PREVIEW3D_MAX_POINTS];
    size_t point_count;
    size_t points_available;   /* before decimation */

    /* Sensor frame; index 0 is MAIN. */
    ui_preview3d_vec3_t sensors[UI_PREVIEW3D_MAX_SENSORS];
    bool sensor_valid[UI_PREVIEW3D_MAX_SENSORS];
    ui_preview3d_source_t sources[UI_PREVIEW3D_MAX_SOURCES];
    size_t source_count;

    bool registered;
    double sensor_to_scan[16];   /* row-major 4x4, valid when registered */
} ui_preview3d_scene_t;

typedef struct
{
    bool drew_scan;
    bool drew_sensors;
    /* Scan and sensors both present but no registration: not overlaid. */
    bool overlay_suppressed_unregistered;
    bool empty;
    uint32_t points_drawn;
    uint32_t lines_drawn;
    uint32_t primitives_clipped;
} ui_preview3d_stats_t;

void ui_preview3d_view_preset(ui_preview3d_view_t *view, ui_preview3d_preset_t preset);

/* LEFT/RIGHT yaw 15 deg, UP/DOWN pitch 10 deg (clamped 0..90), OK cycles
 * TOP -> SIDE -> ISO -> TOP. Returns true when the view changed. */
bool ui_preview3d_view_apply_key(ui_preview3d_view_t *view, ui_preview3d_key_t key);

/* Decimates occupied voxels (voxel centres) into the scene's bounded point
 * list with a uniform stride, and copies a usable registration if the
 * occupancy artifact carries one for this geometry version. */
void ui_preview3d_scene_set_occupancy(
    ui_preview3d_scene_t *scene,
    const scan_occupancy_t *occupancy,
    uint64_t geometry_computed_at_ms);

/* Projects a point (already in the render frame) to pixel coordinates. */
void ui_preview3d_project(
    const ui_preview3d_view_t *view,
    const ui_preview3d_vec3_t *centre,
    float scale,
    int width,
    int height,
    const ui_preview3d_vec3_t *point,
    float *out_x,
    float *out_y);

/* Clears `pixels` (width*height, stride = width) and draws the scene. */
void ui_preview3d_render(
    const ui_preview3d_scene_t *scene,
    const ui_preview3d_view_t *view,
    ui_preview3d_layer_t layer,
    uint8_t *pixels,
    int width,
    int height,
    ui_preview3d_stats_t *stats);

/* Clipped line / point primitives (exposed for tests). Return false when
 * nothing was drawn because the primitive lies outside the buffer. */
bool ui_preview3d_draw_line(uint8_t *pixels, int width, int height,
                            float x0, float y0, float x1, float y1, uint8_t colour);
bool ui_preview3d_plot(uint8_t *pixels, int width, int height, int x, int y, uint8_t colour);
