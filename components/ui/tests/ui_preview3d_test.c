#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan_occupancy.h"
#include "ui_preview3d.h"

/* MAX-187: bounded software 3D preview (host math, clipping, golden). */

#define GUARD 16
#define CANARY 0xA5
#define VOXEL_TEST_BUDGET 3000U
#define SOURCE_VECTORS_PATH "../../../tests/fixtures/source_highlight_vectors.json"
#define CANDIDATE_VOLUME_VECTORS_PATH "../../../tests/fixtures/candidate_volume_vectors.json"
#define SOURCE_VECTOR_ROWS 16U
#define SOURCE_MAPPING_COLUMNS 11U
#define SOURCE_UNCERTAINTY_COLUMNS 22U
#define CANDIDATE_VOLUME_VECTOR_COLUMNS 12U

static uint8_t s_frame[GUARD + 320 * 240 + GUARD];
static ui_preview3d_scene_t s_scene;
static uint32_t s_voxel_order[VOXEL_TEST_BUDGET];

static char *read_vector_fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length >= 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1U);
    assert(text != NULL);
    assert(fread(text, 1U, (size_t)length, file) == (size_t)length);
    text[length] = '\0';
    assert(fclose(file) == 0);
    return text;
}

static char *read_source_vectors(void)
{
    return read_vector_fixture(SOURCE_VECTORS_PATH);
}

static char *read_candidate_volume_vectors(void)
{
    return read_vector_fixture(CANDIDATE_VOLUME_VECTORS_PATH);
}

static uint8_t *read_preview_v2_vector(size_t *length_out)
{
    static const char *const paths[] = {
        "tests/vectors/voxel_preview_v2.bin",
        "../../../tests/vectors/voxel_preview_v2.bin",
    };
    for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *file = fopen(paths[i], "rb");
        if (file == NULL) continue;
        assert(fseek(file, 0L, SEEK_END) == 0);
        const long length = ftell(file);
        assert(length > 0L);
        assert(fseek(file, 0L, SEEK_SET) == 0);
        uint8_t *bytes = malloc((size_t)length);
        assert(bytes != NULL);
        assert(fread(bytes, 1U, (size_t)length, file) == (size_t)length);
        assert(fclose(file) == 0);
        *length_out = (size_t)length;
        return bytes;
    }
    assert(!"registered preview v2 golden vector not found");
    return NULL;
}

static size_t read_source_vector_rows(
    const char *fixture,
    const char *section,
    size_t columns,
    double rows[SOURCE_VECTOR_ROWS][SOURCE_UNCERTAINTY_COLUMNS])
{
    const char *cursor = strstr(fixture, section);
    assert(cursor != NULL);
    cursor = strchr(cursor, '[');
    assert(cursor != NULL);
    ++cursor;
    size_t count = 0U;
    for (;;) {
        while (isspace((unsigned char)*cursor) || *cursor == ',') ++cursor;
        if (*cursor == ']') break;
        assert(*cursor == '[' && count < SOURCE_VECTOR_ROWS);
        ++cursor;
        for (size_t column = 0U; column < columns; ++column) {
            while (isspace((unsigned char)*cursor)) ++cursor;
            char *end = NULL;
            rows[count][column] = strtod(cursor, &end);
            assert(end != cursor);
            cursor = end;
            while (isspace((unsigned char)*cursor)) ++cursor;
            if (column + 1U < columns) {
                assert(*cursor == ',');
                ++cursor;
            }
        }
        assert(*cursor == ']');
        ++cursor;
        ++count;
    }
    return count;
}

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
    assert(!ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_LEFT));
    assert(v.yaw_deg == 0.0f && v.pitch_deg == 90.0f); /* LEFT is Back, never yaw. */
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_OK) && v.preset == UI_PREVIEW3D_PRESET_SIDE && v.pitch_deg == 0.0f);
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_OK) && v.preset == UI_PREVIEW3D_PRESET_ISO);
    assert(fabsf(v.yaw_deg - 45.0f) < 1e-6f && fabsf(v.pitch_deg - 35.26f) < 1e-3f);
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_OK) && v.preset == UI_PREVIEW3D_PRESET_TOP);

    /* Pitch clamps to [0, 90]; a no-op key reports "unchanged". */
    assert(!ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_UP));
    assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_DOWN) && v.pitch_deg == 80.0f);
    assert(v.preset == UI_PREVIEW3D_PRESET_FREE);
    for (int i = 0; i < 8; ++i) {
        assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_DOWN));
    }
    assert(v.pitch_deg == 0.0f);
    assert(!ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_DOWN));
    for (int i = 0; i < 9; ++i) {
        assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_UP));
    }
    assert(v.pitch_deg == 90.0f);
    assert(!ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_UP));

    /* RIGHT advances yaw by 15 degrees and wraps after 24 presses. */
    ui_preview3d_view_preset(&v, UI_PREVIEW3D_PRESET_TOP);
    for (int i = 0; i < 24; ++i) {
        assert(ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_RIGHT));
        assert(fabsf(v.yaw_deg - (float)(((i + 1) * 15) % 360)) < 1e-4f);
    }
    assert(v.yaw_deg == 0.0f);
    assert(!ui_preview3d_view_apply_key(&v, UI_PREVIEW3D_KEY_LEFT) && v.yaw_deg == 0.0f);
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
    static const char glyph[UI_PREVIEW3D_PX_COUNT] = {'.', ':', '-', 'o', 'M', 's', 'S', 'T', 'B', 'F', 'H', 'U', 'X'};

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
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_UNREGISTERED);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_MAIN) == 0 && count_colour(w, h, UI_PREVIEW3D_PX_SCAN) == 2);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SOURCE) == 0 &&
           count_colour(w, h, UI_PREVIEW3D_PX_SOURCE_SELECTED) == 0);

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
    occ.registration.residual_m = 0.004;
    occ.registration.threshold_m = 0.05;
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

static void test_v2_preview_sensor_origin_uses_project_z_up(void)
{
    size_t length = 0U;
    uint8_t *bytes = read_preview_v2_vector(&length);
    scan_preview_t preview;
    assert(scan_preview_parse(bytes, length, NULL, &preview) == SCAN_PREVIEW_OK);
    assert(preview.registration_usable && preview.geometry_computed_at_ms == 123456U);

    ui_preview3d_scene_set_voxel_preview(
        &s_scene, &preview, 123456U, s_voxel_order, VOXEL_TEST_BUDGET);
    assert(s_scene.registered);
    assert(fabs(s_scene.sensor_to_scan[7] + 3.0) < 1e-6);
    assert(fabs(s_scene.sensor_to_scan[11] - 2.0) < 1e-6);
    s_scene.sensor_valid[0] = true;
    s_scene.sensors[0] = (ui_preview3d_vec3_t){0.0f, 0.0f, 0.0f};

    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.drew_scan && stats.drew_sensors && !stats.overlay_suppressed_unregistered);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_MAIN) > 0U);
    /* The sole voxel is centered at the converted sensor origin [0,-3,2]. */
    assert(frame_pixels()[50U * 100U + 50U] == UI_PREVIEW3D_PX_MAIN);
    frame_assert_guards(100, 100);

    scan_preview_free(&preview);
    free(bytes);
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

static void test_localization_source_selection(void)
{
    ui_preview3d_source_t selected[UI_PREVIEW3D_MAX_SOURCES];
    ui_preview3d_source_candidate_t candidates[] = {
        {.method_name = "gauss_newton_tdoa", .computed_at_ms = 10U, .confidence_score = 0.95,
         .position = {1.0f, 0.0f, 0.0f}, .solved = true},
        {.method_name = "joint_fusion", .computed_at_ms = 11U, .confidence_score = 0.40,
         .position = {2.0f, 0.0f, 0.0f}, .joint_fusion = true, .solved = true},
    };
    size_t count = ui_preview3d_select_sources(candidates, 2U, selected, UI_PREVIEW3D_MAX_SOURCES);
    assert(count == 2U && selected[0].selected && selected[0].position.x == 2.0f);
    assert(!selected[1].selected && selected[1].position.x == 1.0f);

    candidates[1].uncertainty_too_high = true;
    candidates[1].confidence_score = 1.0;
    count = ui_preview3d_select_sources(candidates, 2U, selected, UI_PREVIEW3D_MAX_SOURCES);
    assert(count == 2U && selected[0].selected && selected[0].position.x == 1.0f);
    assert(!selected[1].selected && selected[1].position.x == 2.0f);

    candidates[0].computed_at_ms = 12U;
    candidates[1].solved = false;
    count = ui_preview3d_select_sources(candidates, 2U, selected, UI_PREVIEW3D_MAX_SOURCES);
    assert(count == 1U && selected[0].selected && selected[0].position.x == 1.0f);

    candidates[0].solved = false;
    count = ui_preview3d_select_sources(candidates, 2U, selected, UI_PREVIEW3D_MAX_SOURCES);
    assert(count == 0U);

    /* A newer unsolved run supersedes that method's older solved result. */
    candidates[0].solved = true;
    candidates[0].confidence_score = 0.99;
    candidates[1] = (ui_preview3d_source_candidate_t){
        .method_name = "gauss_newton_tdoa", .computed_at_ms = 13U, .confidence_score = 0.0,
        .position = {3.0f, 0.0f, 0.0f}, .solved = false,
    };
    count = ui_preview3d_select_sources(candidates, 2U, selected, UI_PREVIEW3D_MAX_SOURCES);
    assert(count == 0U);
}

static void test_source_to_voxel_mapping(void)
{
    const ui_preview3d_voxel_grid_t grid = {
        .origin = {0.0f, 0.0f, 0.0f},
        .dimensions = {2U, 2U, 2U},
        .voxel_size_m = 1.0f,
    };
    uint32_t index = UINT32_MAX;
    assert(ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.5f, 0.5f, 0.5f}, &index));
    assert(index == 0U);
    assert(ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.0f, 0.0f, 0.0f}, &index));
    assert(index == 0U);
    assert(ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){1.0f, 0.5f, 0.5f}, &index));
    assert(index == 1U);
    assert(!ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){2.0f, 0.5f, 0.5f}, &index));
    assert(!ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){-0.001f, 0.5f, 0.5f}, &index));
    assert(!ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){NAN, 0.5f, 0.5f}, &index));
}

static void test_shared_source_mapping_vectors(void)
{
    char *fixture = read_source_vectors();
    double rows[SOURCE_VECTOR_ROWS][SOURCE_UNCERTAINTY_COLUMNS] = {{0}};
    const size_t count = read_source_vector_rows(fixture, "\"mapping\"", SOURCE_MAPPING_COLUMNS, rows);
    assert(count == 5U);
    for (size_t i = 0U; i < count; ++i) {
        const ui_preview3d_voxel_grid_t grid = {
            .origin = {(float)rows[i][0], (float)rows[i][1], (float)rows[i][2]},
            .dimensions = {(uint16_t)rows[i][3], (uint16_t)rows[i][4], (uint16_t)rows[i][5]},
            .voxel_size_m = (float)rows[i][6],
        };
        const ui_preview3d_vec3_t position = {(float)rows[i][7], (float)rows[i][8], (float)rows[i][9]};
        uint32_t actual = UINT32_MAX;
        const bool mapped = ui_preview3d_source_to_voxel_index(&grid, &position, &actual);
        if (rows[i][10] < 0.0) assert(!mapped);
        else assert(mapped && actual == (uint32_t)rows[i][10]);
    }
    free(fixture);
}

static void test_shared_source_uncertainty_vectors(void)
{
    char *fixture = read_source_vectors();
    double rows[SOURCE_VECTOR_ROWS][SOURCE_UNCERTAINTY_COLUMNS] = {{0}};
    const size_t count = read_source_vector_rows(
        fixture, "\"uncertainty\"", SOURCE_UNCERTAINTY_COLUMNS, rows);
    assert(count == 4U);
    for (size_t row = 0U; row < count; ++row) {
        const ui_preview3d_voxel_grid_t grid = {
            .origin = {(float)rows[row][0], (float)rows[row][1], (float)rows[row][2]},
            .dimensions = {(uint16_t)rows[row][3], (uint16_t)rows[row][4], (uint16_t)rows[row][5]},
            .voxel_size_m = (float)rows[row][6],
        };
        const ui_preview3d_vec3_t position = {
            (float)rows[row][7], (float)rows[row][8], (float)rows[row][9],
        };
        const size_t capacity = (size_t)rows[row][13];
        uint32_t actual[SOURCE_UNCERTAINTY_COLUMNS] = {0U};
        const size_t actual_count = ui_preview3d_source_uncertainty_voxels(
            &grid, &position, (float)rows[row][10], (float)rows[row][11], rows[row][12] != 0.0,
            actual, capacity);
        size_t expected_count = 0U;
        for (size_t column = 14U; column < SOURCE_UNCERTAINTY_COLUMNS && rows[row][column] >= 0.0; ++column) {
            assert(expected_count < SOURCE_UNCERTAINTY_COLUMNS);
            assert(actual_count > expected_count && actual[expected_count] == (uint32_t)rows[row][column]);
            ++expected_count;
        }
        assert(actual_count == expected_count);
    }
    free(fixture);
}

static void test_primary_source_highlights_voxel_faces(void)
{
    static uint32_t indices[] = {0U};
    static uint8_t masks[] = {0x3fU};
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;
    set_voxel_scene(&s_scene, indices, masks, 1U, 2U, 1U, 1U,
                    (ui_preview3d_vec3_t){-0.5f, -0.5f, -0.5f}, 1.0f);
    s_scene.registered = true;
    s_scene.sensor_to_scan[0] = s_scene.sensor_to_scan[5] =
        s_scene.sensor_to_scan[10] = s_scene.sensor_to_scan[15] = 1.0;
    s_scene.sources[0] = (ui_preview3d_source_t){.position = {1.0f, 0.0f, 0.0f}, .selected = true};
    s_scene.source_count = 1U;
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE);
    /* Source cell 1 is absent from the scan's sparse surface list and must
     * still render as a synthetic highlight voxel. */
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT) > 0U);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_TOP) > 0U);

    s_scene.sources[0].position.x = 3.0f;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_OUTSIDE_GRID);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_SOURCE_OUTSIDE_GRID) > 0U);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT) == 0U);

    static uint32_t neighborhood_indices[] = {4U};
    static uint8_t neighborhood_masks[] = {0x3fU};
    set_voxel_scene(&s_scene, neighborhood_indices, neighborhood_masks, 1U, 3U, 3U, 1U,
                    (ui_preview3d_vec3_t){-1.5f, -1.5f, -0.5f}, 1.0f);
    s_scene.registered = true;
    s_scene.sensor_to_scan[0] = s_scene.sensor_to_scan[5] =
        s_scene.sensor_to_scan[10] = s_scene.sensor_to_scan[15] = 1.0;
    s_scene.sources[0] = (ui_preview3d_source_t){
        .position = {0.0f, 0.0f, 0.0f}, .selected = true,
        .horizontal_std_x_m = 1.0f, .horizontal_std_y_m = 1.0f, .uncertainty_valid = true,
    };
    s_scene.source_count = 1U;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT) > 0U);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_UNCERTAINTY) > 0U);
}

static void colour_extent(int width, int height, uint8_t colour, int *extent_x, int *extent_y)
{
    int min_x = width, max_x = -1, min_y = height, max_y = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (frame_pixels()[y * width + x] != colour) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    *extent_x = max_x < 0 ? 0 : max_x - min_x + 1;
    *extent_y = max_y < 0 ? 0 : max_y - min_y + 1;
}

static void set_yaw_registration(double *matrix, double yaw_deg)
{
    const double yaw = yaw_deg * 3.14159265358979323846 / 180.0;
    memset(matrix, 0, 16U * sizeof(*matrix));
    matrix[0] = cos(yaw);
    matrix[1] = -sin(yaw);
    matrix[4] = sin(yaw);
    matrix[5] = cos(yaw);
    matrix[10] = 1.0;
    matrix[15] = 1.0;
}

static void test_uncertainty_support_follows_registration_rotation(void)
{
    double matrix[16];
    double scan_x = -1.0, scan_y = -1.0;

    /* Identity reproduces the sensor-frame sigmas exactly. */
    set_yaw_registration(matrix, 0.0);
    matrix[1] = matrix[4] = 0.0;
    matrix[0] = matrix[5] = 1.0;
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, (double)0.5f, (double)0.1f, 0.0, &scan_x, &scan_y));
    assert(scan_x == (double)0.5f && scan_y == (double)0.1f);
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, (double)0.37f, (double)0.0125f, 0.0, &scan_x, &scan_y));
    assert(scan_x == (double)0.37f && scan_y == (double)0.0125f);

    /* 90 degrees of yaw swaps the axes. */
    set_yaw_registration(matrix, 90.0);
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, 0.50, 0.10, 0.0, &scan_x, &scan_y));
    assert(fabs(scan_x - 0.10) < 1e-9 && fabs(scan_y - 0.50) < 1e-9);

    /* 45 degrees mixes both: sqrt(0.5 * (0.5^2 + 0.1^2)) = 0.3606. */
    set_yaw_registration(matrix, 45.0);
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, 0.50, 0.10, 0.0, &scan_x, &scan_y));
    assert(fabs(scan_x - 0.3606) < 1e-4 && fabs(scan_y - 0.3606) < 1e-4);

    /* A reflection (determinant -1) has the same magnitudes as the rotation. */
    set_yaw_registration(matrix, 0.0);
    matrix[0] = -1.0;
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, 0.50, 0.10, 0.0, &scan_x, &scan_y));
    assert(scan_x == 0.50 && scan_y == 0.10);
    set_yaw_registration(matrix, 90.0);
    matrix[1] = 1.0;
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, 0.50, 0.10, 0.0, &scan_x, &scan_y));
    assert(fabs(scan_x - 0.10) < 1e-9 && fabs(scan_y - 0.50) < 1e-9);

    /* A tilted registration brings the vertical sigma into the scan X axis. */
    memset(matrix, 0, sizeof(matrix));
    matrix[2] = 1.0;
    matrix[5] = 1.0;
    matrix[8] = -1.0;
    matrix[15] = 1.0;
    assert(ui_preview3d_uncertainty_to_scan_frame(matrix, 0.10, 0.20, 0.30, &scan_x, &scan_y));
    assert(fabs(scan_x - 0.30) < 1e-9 && fabs(scan_y - 0.20) < 1e-9);

    /* Invalid input fails closed with zeroed outputs. */
    scan_x = scan_y = 7.0;
    assert(!ui_preview3d_uncertainty_to_scan_frame(NULL, 0.5, 0.1, 0.0, &scan_x, &scan_y));
    assert(scan_x == 0.0 && scan_y == 0.0);
    assert(!ui_preview3d_uncertainty_to_scan_frame(matrix, -0.5, 0.1, 0.0, &scan_x, &scan_y));
    assert(!ui_preview3d_uncertainty_to_scan_frame(matrix, NAN, 0.1, 0.0, &scan_x, &scan_y));
    assert(!ui_preview3d_uncertainty_to_scan_frame(matrix, 0.5, 0.1, INFINITY, &scan_x, &scan_y));
    assert(!ui_preview3d_uncertainty_to_scan_frame(matrix, 0.5, 0.1, 0.0, NULL, &scan_y));
}

static void test_uncertainty_footprint_is_drawn_in_the_scan_frame(void)
{
    static uint32_t indices[] = {0U};
    static uint8_t masks[] = {0x3fU};
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;
    int extent_x = 0, extent_y = 0;

    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);

    /* Identity registration: the long sensor-X sigma stays along scan X. */
    set_voxel_scene(&s_scene, indices, masks, 1U, 21U, 21U, 1U,
                    (ui_preview3d_vec3_t){-1.05f, -1.05f, -0.05f}, 0.1f);
    s_scene.registered = true;
    set_yaw_registration(s_scene.sensor_to_scan, 0.0);
    s_scene.sensor_to_scan[1] = s_scene.sensor_to_scan[4] = 0.0;
    s_scene.sources[0] = (ui_preview3d_source_t){
        .position = {0.0f, 0.0f, 0.0f}, .selected = true,
        .horizontal_std_x_m = 0.5f, .horizontal_std_y_m = 0.1f, .uncertainty_valid = true,
    };
    s_scene.source_count = 1U;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE);
    colour_extent(100, 100, UI_PREVIEW3D_PX_VOXEL_UNCERTAINTY, &extent_x, &extent_y);
    assert(extent_y > 0 && extent_x >= 2 * extent_y);

    /* 90 degree yaw registration: the same sensor-frame sigmas now spread along scan Y. */
    set_yaw_registration(s_scene.sensor_to_scan, 90.0);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE);
    colour_extent(100, 100, UI_PREVIEW3D_PX_VOXEL_UNCERTAINTY, &extent_x, &extent_y);
    assert(extent_x > 0 && extent_y >= 2 * extent_x);
}

static void layout_registered_point_scene(uint64_t registration_geometry)
{
    layout_scene();
    s_scene.points[0] = (ui_preview3d_vec3_t){1.0f, 1.0f, 0.0f};
    s_scene.points[1] = (ui_preview3d_vec3_t){3.0f, 2.0f, 0.0f};
    s_scene.point_count = 2U;
    s_scene.registered = true;
    set_yaw_registration(s_scene.sensor_to_scan, 0.0);
    s_scene.sensor_to_scan[1] = s_scene.sensor_to_scan[4] = 0.0;
    s_scene.registration_geometry_computed_at_ms = registration_geometry;
    s_scene.sources[0].selected = true;
}

static void test_source_geometry_must_match_registration(void)
{
    static uint8_t reference[64 * 48];
    static uint8_t current[64 * 48];
    const int w = 64, h = 48;
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;

    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);

    /* Registration and source from the same geometry (unversioned): drawn. */
    layout_registered_point_scene(0U);
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE && stats.sources_suppressed_stale_geometry == 0U);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SOURCE_SELECTED) > 0);
    memcpy(reference, frame_pixels(), sizeof(reference));

    /* The same geometry generation A/A with a real timestamp draws identically. */
    layout_registered_point_scene(77U);
    s_scene.sources[0].geometry_computed_at_ms = 77U;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE && stats.sources_suppressed_stale_geometry == 0U);
    assert(memcmp(reference, frame_pixels(), sizeof(reference)) == 0);

    /* Source from geometry A against registration B: withheld, with an explicit reason. */
    layout_registered_point_scene(78U);
    s_scene.sources[0].geometry_computed_at_ms = 77U;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_STALE_GEOMETRY);
    assert(stats.sources_suppressed_stale_geometry == 1U);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SOURCE_SELECTED) == 0);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SOURCE) == 0);
    assert(stats.drew_sensors && count_colour(w, h, UI_PREVIEW3D_PX_MAIN) > 0);
    /* Nothing about the withheld source leaks into the picture, not even the view fit. */
    memcpy(current, frame_pixels(), sizeof(current));
    layout_registered_point_scene(78U);
    s_scene.source_count = 0U;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE);
    assert(memcmp(current, frame_pixels(), sizeof(current)) == 0);

    /* A source without a recorded geometry never matches a real registration. */
    layout_registered_point_scene(78U);
    s_scene.sources[0].geometry_computed_at_ms = 0U;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), w, h, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_STALE_GEOMETRY);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SOURCE_SELECTED) == 0);

    /* The sensor-only layer needs no registration, so the source is neither transformed nor withheld. */
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_SENSORS, frame_pixels(), w, h, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE && stats.sources_suppressed_stale_geometry == 0U);
    assert(count_colour(w, h, UI_PREVIEW3D_PX_SOURCE_SELECTED) > 0);
}

static void test_scene_setup_binds_registration_geometry(void)
{
    static uint32_t indices[] = {0U};
    static uint8_t masks[] = {0x3fU};
    scan_preview_t preview = {0};
    static int64_t keys[1];
    scan_occupancy_t occupancy;

    preview.indices = indices;
    preview.face_masks = masks;
    preview.voxel_count = 1U;
    preview.dimensions[0] = preview.dimensions[1] = preview.dimensions[2] = 1U;
    preview.voxel_size_m = 1.0f;
    preview.registration_usable = true;
    preview.geometry_computed_at_ms = 91U;
    preview.sensor_to_scan[0] = preview.sensor_to_scan[5] = preview.sensor_to_scan[10] = preview.sensor_to_scan[15] = 1.0f;
    ui_preview3d_scene_set_voxel_preview(&s_scene, &preview, 91U, s_voxel_order, VOXEL_TEST_BUDGET);
    assert(s_scene.registered && s_scene.registration_geometry_computed_at_ms == 91U);
    ui_preview3d_scene_set_voxel_preview(&s_scene, &preview, 92U, s_voxel_order, VOXEL_TEST_BUDGET);
    assert(!s_scene.registered && s_scene.registration_geometry_computed_at_ms == 0U);

    memset(&occupancy, 0, sizeof(occupancy));
    keys[0] = pack(0, 0, 0);
    occupancy.voxel_size_m = 0.1;
    occupancy.occupied_keys = keys;
    occupancy.occupied_count = 1U;
    occupancy.registration.present = true;
    occupancy.registration.geometry_computed_at_ms = 93U;
    occupancy.registration.residual_m = 0.004;
    occupancy.registration.threshold_m = 0.05;
    occupancy.registration.sensor_to_scan[0] = occupancy.registration.sensor_to_scan[5] = 1.0;
    occupancy.registration.sensor_to_scan[10] = occupancy.registration.sensor_to_scan[15] = 1.0;
    ui_preview3d_scene_set_occupancy(&s_scene, &occupancy, 93U);
    assert(s_scene.registered && s_scene.registration_geometry_computed_at_ms == 93U);
    ui_preview3d_scene_set_occupancy(&s_scene, &occupancy, 94U);
    assert(!s_scene.registered && s_scene.registration_geometry_computed_at_ms == 0U);
}

static void test_candidates_carry_geometry_and_vertical_sigma(void)
{
    ui_preview3d_source_t selected[UI_PREVIEW3D_MAX_SOURCES];
    ui_preview3d_source_candidate_t candidate = {
        .method_name = "semblance", .computed_at_ms = 10U, .confidence_score = 0.9,
        .position = {1.0f, 2.0f, 3.0f}, .horizontal_std_x_m = 0.5, .horizontal_std_y_m = 0.25,
        .vertical_std_m = 0.125, .geometry_computed_at_ms = 55U, .solved = true, .uncertainty_valid = true,
    };
    assert(ui_preview3d_select_sources(&candidate, 1U, selected, UI_PREVIEW3D_MAX_SOURCES) == 1U);
    assert(selected[0].geometry_computed_at_ms == 55U && selected[0].vertical_std_m == 0.125f);
    assert(selected[0].uncertainty_valid && selected[0].horizontal_std_x_m == 0.5f);

    candidate.vertical_std_m = NAN;
    assert(ui_preview3d_select_sources(&candidate, 1U, selected, UI_PREVIEW3D_MAX_SOURCES) == 1U);
    assert(selected[0].vertical_std_m == 0.0f && selected[0].uncertainty_valid);
    candidate.vertical_std_m = -1.0;
    assert(ui_preview3d_select_sources(&candidate, 1U, selected, UI_PREVIEW3D_MAX_SOURCES) == 1U);
    assert(selected[0].vertical_std_m == 0.0f);
}

static void test_buried_source_maps_into_extended_preview_grid(void)
{
    /* MAX-413: surface voxels occupy z from 0 to 0.5; the grid was extended down to z = -2.0. */
    static uint32_t indices[] = {8U};
    static uint8_t masks[] = {0x3fU};
    const ui_preview3d_voxel_grid_t grid = {
        .origin = {-0.25f, -0.25f, -2.0f}, .dimensions = {2U, 1U, 5U}, .voxel_size_m = 0.5f,
    };
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;
    uint32_t index = UINT32_MAX;

    assert(ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.5f, 0.0f, -1.0f}, &index));
    assert(index == 5U);
    assert(ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.5f, 0.0f, -2.0f}, &index));
    assert(index == 1U);
    assert(ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){-0.25f, 0.0f, 0.4999f}, &index));
    assert(index == 8U);
    assert(!ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.5f, 0.0f, -2.5f}, &index));
    assert(!ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.5f, 0.0f, -2.0001f}, &index));
    assert(!ui_preview3d_source_to_voxel_index(&grid, &(ui_preview3d_vec3_t){0.5f, 0.0f, 0.5f}, &index));

    set_voxel_scene(&s_scene, indices, masks, 1U, 2U, 1U, 5U, grid.origin, grid.voxel_size_m);
    s_scene.registered = true;
    set_yaw_registration(s_scene.sensor_to_scan, 0.0);
    s_scene.sensor_to_scan[1] = s_scene.sensor_to_scan[4] = 0.0;
    s_scene.sources[0] = (ui_preview3d_source_t){.position = {0.5f, 0.0f, -1.0f}, .selected = true};
    s_scene.source_count = 1U;
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);

    /* In range: a highlight cell is synthesized; the occupied voxel list is untouched. */
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_NONE);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT) > 0);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_SOURCE_OUTSIDE_GRID) == 0);
    assert(s_scene.voxel_count == 1U && s_scene.voxel_indices[0] == 8U);
    assert(stats.voxels_drawn == 2U);

    /* Below the extended grid: still reported as outside, without a highlight cell. */
    s_scene.sources[0].position.z = -2.5f;
    frame_reset();
    ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
    assert(stats.source_reason == UI_PREVIEW3D_SOURCE_REASON_OUTSIDE_GRID);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_VOXEL_HIGHLIGHT) == 0);
    assert(count_colour(100, 100, UI_PREVIEW3D_PX_SOURCE_OUTSIDE_GRID) > 0);
    assert(stats.voxels_drawn == 1U);
}

static void test_shared_candidate_volume_vectors(void)
{
    static const uint32_t indices[] = {0U};
    static const uint8_t masks[] = {0x3fU};
    char *fixture = read_candidate_volume_vectors();
    double rows[SOURCE_VECTOR_ROWS][SOURCE_UNCERTAINTY_COLUMNS] = {{0.0}};
    const size_t count = read_source_vector_rows(
        fixture, "\"vectors\"", CANDIDATE_VOLUME_VECTOR_COLUMNS, rows);
    ui_preview3d_view_t view;
    ui_preview3d_stats_t stats;

    assert(count > 0U);
    ui_preview3d_view_preset(&view, UI_PREVIEW3D_PRESET_TOP);
    for (size_t vector = 0U; vector < count; ++vector) {
        const double *row = rows[vector];
        const double envelope_min[3] = {row[0], row[1], row[2]};
        const double envelope_max[3] = {row[3], row[4], row[5]};
        const double below_surface_m = row[6];
        const double voxel_size_m = row[7];
        const double point[3] = {row[8], row[9], row[10]};
        const bool expected_inside = row[11] == 1.0;
        scan_occupancy_t occupancy = {0};
        ui_preview3d_voxel_grid_t grid = {0};
        ui_preview3d_vec3_t preview_point = {
            (float)point[0], (float)point[1], (float)point[2],
        };
        uint32_t index = UINT32_MAX;

        assert(row[11] == 0.0 || row[11] == 1.0);
        occupancy.voxel_size_m = voxel_size_m;
        occupancy.below_surface_m = below_surface_m;
        occupancy.has_envelope = true;
        memcpy(occupancy.envelope_min, envelope_min, sizeof(envelope_min));
        memcpy(occupancy.envelope_max, envelope_max, sizeof(envelope_max));
        assert(scan_occupancy_classify(&occupancy, point) == (expected_inside
            ? SCAN_OCCUPANCY_UNKNOWN
            : SCAN_OCCUPANCY_OUTSIDE));

        grid.origin = (ui_preview3d_vec3_t){
            (float)envelope_min[0],
            (float)envelope_min[1],
            (float)(envelope_min[2] - below_surface_m),
        };
        grid.voxel_size_m = (float)voxel_size_m;
        for (size_t axis = 0U; axis < 3U; ++axis) {
            const double lower = axis == 2U ? envelope_min[axis] - below_surface_m : envelope_min[axis];
            const double span_cells = (envelope_max[axis] - lower) / voxel_size_m;
            const double dimension = floor(span_cells) + 1.0;
            assert(span_cells >= 0.0 && fabs(span_cells - floor(span_cells)) < 1e-9);
            assert(dimension > 0.0 && dimension <= (double)UINT16_MAX);
            grid.dimensions[axis] = (uint16_t)dimension;
        }
        assert(ui_preview3d_source_to_voxel_index(&grid, &preview_point, &index) == expected_inside);

        set_voxel_scene(&s_scene, indices, masks, 1U, grid.dimensions[0], grid.dimensions[1],
                        grid.dimensions[2], grid.origin, grid.voxel_size_m);
        s_scene.registered = true;
        set_yaw_registration(s_scene.sensor_to_scan, 0.0);
        s_scene.sources[0] = (ui_preview3d_source_t){.position = preview_point, .selected = true};
        s_scene.source_count = 1U;
        frame_reset();
        ui_preview3d_render(&s_scene, &view, UI_PREVIEW3D_LAYER_AUTO, frame_pixels(), 100, 100, &stats);
        assert(stats.source_reason == (expected_inside
            ? UI_PREVIEW3D_SOURCE_REASON_NONE
            : UI_PREVIEW3D_SOURCE_REASON_OUTSIDE_GRID));
    }
    free(fixture);
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
    test_v2_preview_sensor_origin_uses_project_z_up();
    test_voxel_viewport_clipping_far_depth_and_budget();
    test_localization_source_selection();
    test_source_to_voxel_mapping();
    test_shared_source_mapping_vectors();
    test_shared_source_uncertainty_vectors();
    test_primary_source_highlights_voxel_faces();
    test_uncertainty_support_follows_registration_rotation();
    test_uncertainty_footprint_is_drawn_in_the_scan_frame();
    test_source_geometry_must_match_registration();
    test_scene_setup_binds_registration_geometry();
    test_candidates_carry_geometry_and_vertical_sigma();
    test_buried_source_maps_into_extended_preview_grid();
    test_shared_candidate_volume_vectors();
    puts("ui_preview3d tests passed");
    return 0;
}
