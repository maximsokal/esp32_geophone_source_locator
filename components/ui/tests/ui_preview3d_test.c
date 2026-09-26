#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_preview3d.h"

/* MAX-187: bounded software 3D preview (host math, clipping, golden). */

#define GUARD 16
#define CANARY 0xA5
#define VOXEL_TEST_BUDGET 3000U

static uint8_t s_frame[GUARD + 320 * 240 + GUARD];
static ui_preview3d_scene_t s_scene;
static uint32_t s_voxel_order[VOXEL_TEST_BUDGET];

static uint8_t *frame_pixels(void)
{
    return s_frame + GUARD;
}

static void frame_reset(void)
{
    memset(s_frame, CANARY, sizeof(s_frame));
}

static void frame_assert_guards(int width, int height)
{
    for (int i = 0; i < GUARD; ++i) {
        assert(s_frame[i] == CANARY);
        assert(s_frame[GUARD + width * height + i] == CANARY);
    }
}

static int count_colour(int width, int height, uint8_t colour)
{
    int n = 0;
    for (int i = 0; i < width * height; ++i) {
        n += frame_pixels()[i] == colour;
    }
    return n;
}

static void assert_colour_near_projection(const ui_preview3d_view_t *view, const ui_preview3d_vec3_t *point,
                                          float scale, int width, int height, uint8_t colour)
{
    float x, y;
    bool found = false;
    ui_preview3d_project(view, &(ui_preview3d_vec3_t){0}, scale, width, height, point, &x, &y);
    const int cx = (int)lroundf(x), cy = (int)lroundf(y);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (cx + dx >= 0 && cy + dy >= 0 && cx + dx < width && cy + dy < height &&
                frame_pixels()[(cy + dy) * width + cx + dx] == colour) found = true;
        }
    }
    assert(found);
}

static void test_presets_and_keys(void)
{
    ui_preview3d_view_t v;
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);
    assert(v.pitch_deg == 90.0f && v.yaw_deg == 0.0f && v.zoom == 1.0f);
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_OK) && v.preset == UI_PREVIEW3D_PRESET_SIDE && v.pitch_deg == 0.0f);
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_OK) && v.preset == UI_PREVIEW3D_PRESET_ISO);
    assert(fabsf(v.yaw_deg - 45.0f) < 1e-6f && fabsf(v.pitch_deg - 35.26f) < 1e-3f);
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_OK) && v.preset == UI_PREVIEW3D_PRESET_TOP);

    /* Pitch clamps to [0, 90]; a no-op key reports "unchanged". */
    assert(!ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_UP));
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_DOWN) && v.pitch_deg == 80.0f);
    assert(v.preset == UI_PREVIEW3D_PRESET_FREE);
    for (int i = 0; i < 20; ++i) {
        (void)ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_DOWN);
    }
    assert(v.pitch_deg == 0.0f);

    /* Yaw wraps into [-180, 180). */
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);
    for (int i = 0; i < 13; ++i) {
        assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_RIGHT));
    }
    assert(fabsf(v.yaw_deg - (-165.0f)) < 1e-4f);
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_LEFT) && fabsf(v.yaw_deg - 180.0f) > 1e-3f);
    assert(!ui_preview3d_view_apply_key(NULL, UI_PREVIEW3D_KEY_OK));
}

static void test_projection_axes(void)
{
    const ui_preview3d_vec3_t c = {0};
    const ui_preview3d_vec3_t px = {1, 0, 0}, py = {0, 1, 0}, pz = {0, 0, 1};
    ui_preview3d_view_t v;
    float x, y;

    /* TOP: +X right, +Y up (screen y decreases), Z invisible. */
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);
    ui_preview3d_project(&v, &c, 10.0f, 100, 80, &px, &x, &y);
    assert(fabsf(x - 60.0f) < 1e-4f && fabsf(y - 40.0f) < 1e-4f);
    ui_preview3d_project(&v, &c, 10.0f, 100, 80, &py, &x, &y);
    assert(fabsf(x - 50.0f) < 1e-4f && fabsf(y - 30.0f) < 1e-4f);
    ui_preview3d_project(&v, &c, 10.0f, 100, 80, &pz, &x, &y);
    assert(fabsf(x - 50.0f) < 1e-4f && fabsf(y - 40.0f) < 1e-4f);

    /* SIDE: +Z up, Y is depth. */
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_SIDE);
    ui_preview3d_project(&v, &c, 10.0f, 100, 80, &pz, &x, &y);
    assert(fabsf(x - 50.0f) < 1e-4f && fabsf(y - 30.0f) < 1e-4f);
    ui_preview3d_project(&v, &c, 10.0f, 100, 80, &py, &x, &y);
    assert(fabsf(x - 50.0f) < 1e-4f && fabsf(y - 40.0f) < 1e-4f);

    /* TOP rotated by yaw 90: the scene turns, +Y now points right. */
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);
    v.yaw_deg = 90.0f;
    ui_preview3d_project(&v, &c, 10.0f, 100, 80, &py, &x, &y);
    assert(fabsf(x - 60.0f) < 1e-3f && fabsf(y - 40.0f) < 1e-3f);
}

static void test_line_clipping(void)
{
    const int w = 20, h = 10;

    frame_reset();
    memset(frame_pixels(), 0, (size_t)(w * h));
    /* Fully outside: nothing drawn, buffer untouched. */
    assert(!ui_preview3d_draw_line(frame_pixels(), w, h, -30, -5, -2, -40, 7));
    assert(!ui_preview3d_draw_line(frame_pixels(), w, h, 25, 3, 90, 8, 7));
    assert(count_colour(w, h, 7) == 0);
    /* Crossing the whole buffer: exactly one full row. */
    assert(ui_preview3d_draw_line(frame_pixels(), w, h, -100, 5, 1000, 5, 7));
    assert(count_colour(w, h, 7) == w);
    for (int x = 0; x < w; ++x) {
        assert(frame_pixels()[5 * w + x] == 7);
    }
    /* Diagonal entering and leaving through corners stays in bounds. */
    assert(ui_preview3d_draw_line(frame_pixels(), w, h, -50, -50, 60, 60, 3));
    assert(frame_pixels()[0] == 3 && frame_pixels()[9 * w + 9] == 3);
    /* Non-finite input is rejected. */
    assert(!ui_preview3d_draw_line(frame_pixels(), w, h, NAN, 0, 5, 5, 9));
    assert(!ui_preview3d_draw_line(frame_pixels(), w, h, 0, 0, INFINITY, 5, 9));
    assert(count_colour(w, h, 9) == 0);
    assert(!ui_preview3d_plot(frame_pixels(), w, h, w, 0, 1));
    assert(!ui_preview3d_plot(frame_pixels(), w, h, 0, -1, 1));
    frame_assert_guards(w, h);
}

static void layout_scene(void)
{
    memset(&s_scene, 0, sizeof(s_scene));
    s_scene.sensors[0] = (ui_preview3d_vec3_t){0, 0, 0};
    s_scene.sensors[1] = (ui_preview3d_vec3_t){4, 0, 0};
    s_scene.sensors[2] = (ui_preview3d_vec3_t){0, 3, 0};
    s_scene.sensor_valid[0] = s_scene.sensor_valid[1] = s_scene.sensor_valid[2] = true;
    s_scene.sources[0] = (ui_preview3d_source_t){{2.0f, 1.0f, -0.5f}, 0.6f, true};
    s_scene.source_count = 1;
}

static const char *const GOLDEN_TOP[] = {
#include "ui_preview3d_golden_top.inc"
};

static void test_golden_top_view(void)
{
    const int w = 32, h = 16;
    ui_preview3d_view_t v;
    ui_preview3d_stats_t stats;
    static const char glyph[UI_PREVIEW3D_PX_COUNT] = {'.', ':', '-', 'o', 'M', 's', 'S', 'T', 'B', 'F'};

    layout_scene();
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);
    frame_reset();
    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    frame_assert_guards(w, h);
    assert(stats.drew_sensors && !stats.drew_scan && !stats.overlay_suppressed_unregistered && !stats.empty);
    assert(stats.lines_drawn == 2 && stats.primitives_clipped == 0);

    for (int y = 0; y < h; ++y) {
        char row[33];
        for (int x = 0; x < w; ++x) {
            row[x] = glyph[frame_pixels()[y * w + x]];
        }
        row[w] = '\0';
        if (getenv("UI_PREVIEW3D_PRINT_GOLDEN") != NULL) {
            printf("    \"%s\",\n", row);
        } else {
            assert(strcmp(row, GOLDEN_TOP[y]) == 0);
        }
    }
}

static void test_frames_are_not_mixed_without_registration(void)
{
    const int w = 64, h = 48;
    ui_preview3d_view_t v;
    ui_preview3d_stats_t stats;

    layout_scene();
    s_scene.points[0] = (ui_preview3d_vec3_t){10, 0, 0};
    s_scene.points[1] = (ui_preview3d_vec3_t){12, 2, 1};
    s_scene.point_count = 2;
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);

    frame_reset();
    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.drew_scan && !stats.drew_sensors && stats.overlay_suppressed_unregistered);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_MAIN) == 0 && count_colour(w, h, UI_PREVIEW3D_PX_SCAN) == 2);

    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_SENSORS, frame_pixels(), w, h, &stats);
    assert(!stats.drew_scan && stats.drew_sensors && !stats.overlay_suppressed_unregistered);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SCAN) == 0 && count_colour(w, h, UI_PREVIEW3D_PX_MAIN) > 0);

    /* Registered (pure translation by +10 m in X): MAIN lands on the scan
     * point at (10, 0, 0) and both layers are drawn. */
    s_scene.registered = true;
    memset(s_scene.sensor_to_scan, 0, sizeof(s_scene.sensor_to_scan));
    s_scene.sensor_to_scan[0] = s_scene.sensor_to_scan[5] = s_scene.sensor_to_scan[10] = s_scene.sensor_to_scan[15] = 1.0;
    s_scene.sensor_to_scan[3] = 10.0;
    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.drew_scan && stats.drew_sensors && !stats.overlay_suppressed_unregistered);
    /* The MAIN marker covers the first scan point; the other remains visible. */
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SCAN) == 1);
    frame_assert_guards(w, h);
}

static void test_empty_and_degenerate_scenes(void)
{
    const int w = 40, h = 30;
    ui_preview3d_view_t v;
    ui_preview3d_stats_t stats;

    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_ISO);
    memset(&s_scene, 0, sizeof(s_scene));
    frame_reset();
    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.empty && count_colour(w, h, UI_PREVIEW3D_PX_BACKGROUND) == w * h);

    /* A single MAIN node still renders (minimum radius), centred. */
    s_scene.sensor_valid[0] = true;
    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(!stats.empty && frame_pixels()[15 * w + 20] == UI_PREVIEW3D_PX_MAIN);

    /* Huge source radius and far-away sensors: clipped, never out of bounds. */
    layout_scene();
    s_scene.sources[0].radius_m = 1e6f;
    s_scene.sensors[3] = (ui_preview3d_vec3_t){1e7f, -1e7f, 5e6f};
    s_scene.sensor_valid[3] = true;
    v.zoom = 50.0f;
    ui_preview3d_render(&s_scene, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    frame_assert_guards(w, h);
    assert(stats.primitives_clipped > 0);
    ui_preview3d_render(NULL, &v, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.empty);
}

static int64_t pack(int32_t ix, int32_t iy, int32_t iz)
{
    return ((int64_t)(ix + 32768) << 32) | ((int64_t)(iy + 32768) << 16) | (int64_t)(iz + 32768);
}

static void test_occupancy_decimation_and_registration(void)
{
    static int64_t keys[4000];
    scan_occupancy_t occ;

    memset(&occ, 0, sizeof(occ));
    for (int i = 0; i < 4000; ++i) {
        keys[i] = pack(i % 50 - 25, i / 50 - 40, -3);
    }
    occ.voxel_size_m = 0.1;
    occ.occupied_keys = keys;
    occ.occupied_count = 4000;

    memset(&s_scene, 0, sizeof(s_scene));
    ui_preview3d_scene_set_occupancy(&s_scene, &occ, 77);
    assert(s_scene.points_available == 4000);
    assert(s_scene.point_count > 1000 && s_scene.point_count <= UI_PREVIEW3D_MAX_POINTS);
    /* Voxel centres, negative indices included. */
    assert(fabsf(s_scene.points[0].x - (-2.45f)) < 1e-5f);
    assert(fabsf(s_scene.points[0].y - (-3.95f)) < 1e-5f);
    assert(fabsf(s_scene.points[0].z - (-0.25f)) < 1e-5f);
    assert(!s_scene.registered);
    /* Uniform decimation spans the whole artifact, not just its first rows. */
    {
        float max_y = -1e9f;
        for (size_t i = 0; i < s_scene.point_count; ++i) {
            max_y = fmaxf(max_y, s_scene.points[i].y);
        }
        assert(max_y > 3.5f);
    }

    /* Registration only for the matching geometry version. */
    occ.registration.present = true;
    occ.registration.geometry_computed_at_ms = 77;
    occ.registration.sensor_to_scan[0] = occ.registration.sensor_to_scan[5] = 1.0;
    occ.registration.sensor_to_scan[10] = occ.registration.sensor_to_scan[15] = 1.0;
    ui_preview3d_scene_set_occupancy(&s_scene, &occ, 78);
    assert(!s_scene.registered);
    ui_preview3d_scene_set_occupancy(&s_scene, &occ, 77);
    assert(s_scene.registered && s_scene.sensor_to_scan[15] == 1.0);

    /* Small artifacts keep every voxel; missing ones clear the layer. */
    occ.occupied_count = 7;
    ui_preview3d_scene_set_occupancy(&s_scene, &occ, 77);
    assert(s_scene.point_count == 7);
    ui_preview3d_scene_set_occupancy(&s_scene, NULL, 77);
    assert(s_scene.point_count == 0 && !s_scene.registered);
}

static void set_voxel_scene(
    ui_preview3d_scene_t *scene,
    const uint32_t *indices,
    const uint8_t *masks,
    uint32_t count,
    uint16_t dx,
    uint16_t dy,
    uint16_t dz,
    ui_preview3d_vec3_t origin,
    float voxel_size)
{
    memset(scene, 0, sizeof(*scene));
    scene->voxel_indices = indices;
    scene->voxel_face_masks = masks;
    scene->voxel_count = count;
    scene->voxel_dimensions[0] = dx;
    scene->voxel_dimensions[1] = dy;
    scene->voxel_dimensions[2] = dz;
    scene->voxel_origin = origin;
    scene->voxel_size_m = voxel_size;
    scene->voxel_order = s_voxel_order;
    scene->voxel_order_capacity = VOXEL_TEST_BUDGET;
}

static void test_voxel_cube_faces_and_projected_corners(void)
{
    static uint32_t index[] = {0U};
    static uint8_t mask[] = {0x3fU};
    const ui_preview3d_vec3_t origin = {-0.5f, -0.5f, -0.5f};
    const int w = 100, h = 100;
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;
    float scale = (0.9f * 0.5f * (float)w) / (sqrtf(3.0f) * 0.5f);

    set_voxel_scene(&s_scene, index, mask, 1U, 1U, 1U, 1U, origin, 1.0f);
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.drew_scan && stats.voxels_drawn == 1U && stats.faces_drawn == 1U);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_TOP) > 0);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_SIDE) == 0);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_FRONT) == 0);
    {
        const ui_preview3d_vec3_t corners[] = {{-0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}};
        float x, y;
        ui_preview3d_project(&view, &(ui_preview3d_vec3_t){0}, scale, w, h, &corners[0], &x, &y);
        assert(fabsf(x - (50.0f - scale * 0.5f)) < 1e-4f && fabsf(y - (50.0f + scale * 0.5f)) < 1e-4f);
        ui_preview3d_project(&view, &(ui_preview3d_vec3_t){0}, scale, w, h, &corners[1], &x, &y);
        assert(fabsf(x - (50.0f + scale * 0.5f)) < 1e-4f && fabsf(y - (50.0f - scale * 0.5f)) < 1e-4f);
        assert_colour_near_projection(&view, &corners[0], scale, w, h, UI_PREVIEW3D_PX_VOXEL_TOP);
    }

    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_SIDE);
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.voxels_drawn == 1U && stats.faces_drawn == 1U);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_FRONT) > 0);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_TOP) == 0);
    {
        const ui_preview3d_vec3_t corner = {-0.5f, -0.5f, 0.5f};
        float x, y;
        ui_preview3d_project(&view, &(ui_preview3d_vec3_t){0}, scale, w, h, &corner, &x, &y);
        assert(fabsf(x - (50.0f - scale * 0.5f)) < 1e-4f && fabsf(y - (50.0f - scale * 0.5f)) < 1e-4f);
        assert_colour_near_projection(&view, &corner, scale, w, h, UI_PREVIEW3D_PX_VOXEL_FRONT);
    }

    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_ISO);
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.voxels_drawn == 1U && stats.faces_drawn == 3U);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_TOP) > 0);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_SIDE) > 0);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_VOXEL_FRONT) > 0);
    {
        const ui_preview3d_vec3_t corner = {-0.5f, -0.5f, 0.5f};
        const ui_preview3d_vec3_t top = {0.0f, 0.0f, 0.5f};
        const ui_preview3d_vec3_t side = {0.5f, 0.0f, 0.0f};
        const ui_preview3d_vec3_t front = {0.0f, -0.5f, 0.0f};
        const float yaw = view.yaw_deg * 0.017453292519943295f;
        const float pitch = view.pitch_deg * 0.017453292519943295f;
        const float qx = -0.5f * cosf(yaw) - 0.5f * sinf(yaw);
        float x, y;
        ui_preview3d_project(&view, &(ui_preview3d_vec3_t){0}, scale, w, h, &corner, &x, &y);
        assert(fabsf(x - (50.0f + scale * qx)) < 1e-4f);
        assert(fabsf(y - (50.0f - scale * (0.5f * cosf(pitch)))) < 1e-4f);
        assert_colour_near_projection(&view, &top, scale, w, h, UI_PREVIEW3D_PX_VOXEL_TOP);
        assert_colour_near_projection(&view, &side, scale, w, h, UI_PREVIEW3D_PX_VOXEL_SIDE);
        assert_colour_near_projection(&view, &front, scale, w, h, UI_PREVIEW3D_PX_VOXEL_FRONT);
    }
    frame_assert_guards(w, h);
}

static void test_voxel_depth_order_and_registration_binding(void)
{
    static uint32_t indices[] = {2U, 5U};
    static uint8_t masks[] = {1U << 4U, 1U << 3U};
    scan_preview_t preview = {0};
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;

    preview.indices = indices;
    preview.face_masks = masks;
    preview.voxel_count = 2U;
    preview.dimensions[0] = preview.dimensions[1] = preview.dimensions[2] = 2U;
    preview.origin[0] = preview.origin[1] = preview.origin[2] = -0.5f;
    preview.voxel_size_m = 1.0f;
    preview.registration_usable = true;
    preview.geometry_computed_at_ms = 77U;
    preview.sensor_to_scan[0] = preview.sensor_to_scan[5] = preview.sensor_to_scan[10] = preview.sensor_to_scan[15] = 1.0f;
    ui_preview3d_scene_set_voxel_preview(&s_scene, &preview, 77U, s_voxel_order, VOXEL_TEST_BUDGET);
    assert(s_scene.registered);
    ui_preview3d_scene_set_voxel_preview(&s_scene, &preview, 78U, s_voxel_order, VOXEL_TEST_BUDGET);
    assert(!s_scene.registered);
    s_scene.sensor_valid[0] = true;
    s_scene.sensors[0] = (ui_preview3d_vec3_t){0};
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_ISO);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.overlay_suppressed_unregistered && !stats.drew_sensors);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_MAIN) == 0);
    ui_preview3d_scene_set_voxel_preview(&s_scene, &preview, 77U, s_voxel_order, VOXEL_TEST_BUDGET);

    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.drew_sensors && !stats.overlay_suppressed_unregistered);
    /* Same projected centers; closest voxel is first after the ascending sort. */
    assert(s_voxel_order[0] == 1U && s_voxel_order[1] == 0U);
    assert(stats.faces_drawn == 2U && stats.voxels_drawn == 2U);
    frame_assert_guards(100, 100);
}

static void test_voxel_viewport_clipping_far_depth_and_budget(void)
{
    static uint32_t one_index[] = {0U};
    static uint8_t one_mask[] = {0x3fU};
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;
    set_voxel_scene(&s_scene, one_index, one_mask, 1U, 1U, 1U, 1U, (ui_preview3d_vec3_t){-0.5f, -0.5f, -0.5f}, 1.0f);
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    view.zoom = 8.0f;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 64, 48, &stats);
    assert(stats.faces_drawn == 1U && stats.primitives_clipped > 0U);
    assert(count_colour(64, 48, UI_PREVIEW3D_PX_VOXEL_TOP) > 0);
    frame_assert_guards(64, 48);

    /* Orthographic projection has no perspective divide for far-side voxels. */
    set_voxel_scene(&s_scene, one_index, one_mask, 1U, 1U, 1U, 1U, (ui_preview3d_vec3_t){0.0f, 0.0f, -1.0e20f}, 1.0e20f);
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 64, 48, &stats);
    assert(stats.faces_drawn == 1U && stats.drew_scan);
    frame_assert_guards(64, 48);

    static uint32_t indices[VOXEL_TEST_BUDGET];
    static uint8_t masks[VOXEL_TEST_BUDGET];
    for (uint32_t i = 0U; i < VOXEL_TEST_BUDGET; ++i) {
        indices[i] = i;
        masks[i] = 0x3fU;
    }
    set_voxel_scene(&s_scene, indices, masks, VOXEL_TEST_BUDGET, 30U, 10U, 10U, (ui_preview3d_vec3_t){0}, 1.0f);
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 320, 240, &stats);
    assert(stats.drew_scan && stats.voxels_drawn == VOXEL_TEST_BUDGET);
    assert(stats.faces_drawn == VOXEL_TEST_BUDGET);
    frame_assert_guards(320, 240);
}

int main(void)
{
    test_presets_and_keys();
    test_projection_axes();
    test_line_clipping();
    test_golden_top_view();
    test_frames_are_not_mixed_without_registration();
    test_empty_and_degenerate_scenes();
    test_occupancy_decimation_and_registration();
    test_voxel_cube_faces_and_projected_corners();
    test_voxel_depth_order_and_registration_binding();
    test_voxel_viewport_clipping_far_depth_and_budget();
    puts("ui_preview3d tests passed");
    return 0;
}
