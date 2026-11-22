#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "seismic_recorder_geometry_policy.h"

/*
 * The geometry of a localization built from the phone's tag layout, as an ALTERNATIVE to the Channel Sounding / barometer layout: only tagged
 * remotes take part, one source for all of them, never a mix; the stored layout is only read; and a layout that would lower the result's
 * dimensionality is not used.
 */

#define GEOMETRY_AT_MS 4000U

static seismic_recorder_session_participation_t session(unsigned assigned_pods)
{
    seismic_recorder_session_participation_t participation;
    memset(&participation, 0, sizeof(participation));
    for (unsigned i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        participation.pods[i] = (seismic_recorder_pod_participation_t){
            .assigned = i < assigned_pods, .node_id_known = i < assigned_pods, .node_id = (uint8_t)(i + 1U),
        };
    }
    return participation;
}

/* Channel Sounding / barometer layout of `pods` remotes (deliberately different from the tag positions). */
static object_store_geometry_t cs_geometry(unsigned pods)
{
    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    for (unsigned i = 0U; i < pods; ++i) {
        geometry.pods[i] = (object_store_pod_geometry_t){
            .computed = true, .leveled = (i % 2U) == 0U, .x_mm = (int32_t)(7 * (i + 1U)), .y_mm = (int32_t)(9 * (i + 1U)), .z_mm = 300,
            .residual_mm = 40, .uncertainty_mm = 150, .computed_at_ms = GEOMETRY_AT_MS + (i + 1U == pods ? 3U : i),
        };
    }
    return geometry;
}

/* MAIN at the origin and the remotes in `nodes` (node ids 1..4) at the given millimetre positions. */
static seismic_recorder_tag_layout_t layout_of(const unsigned *nodes, size_t count)
{
    static const int32_t xyz[5][3] = {{0, 0, 0}, {500, 0, 0}, {0, 800, 0}, {-100, 500, 200}, {900, 900, 50}};
    seismic_recorder_tag_layout_t layout;
    memset(&layout, 0, sizeof(layout));
    layout.available = true;
    layout.weakest_main_triangle_height_mm = 400;
    layout.geometry_computed_at_ms = GEOMETRY_AT_MS + 3U;   /* the newest computed_at_ms of cs_geometry() */
    layout.main_scan_mm[0] = 1000; layout.main_scan_mm[1] = 2000; layout.main_scan_mm[2] = 1000;
    layout.nodes[0] = (seismic_recorder_tag_node_t){.valid = true, .node_id = 0U, .tag_id = 0U, .confirmed = true, .observations = 9U, .views = 4U};
    layout.node_count = 1U;
    for (size_t i = 0U; i < count; ++i) {
        const unsigned node = nodes[i];
        layout.nodes[node] = (seismic_recorder_tag_node_t){
            .valid = true, .node_id = (uint8_t)node, .tag_id = (uint8_t)node, .x_mm = xyz[node][0], .y_mm = xyz[node][1], .z_mm = xyz[node][2],
            .uncertainty_mm = (int32_t)(4 + node), .confirmed = (node % 2U) == 1U, .observations = 8U, .views = 3U,
        };
        ++layout.node_count;
    }
    return layout;
}

static bool equal(const object_store_geometry_t *a, const object_store_geometry_t *b) { return memcmp(a, b, sizeof(*a)) == 0; }

static void a_full_layout_becomes_the_geometry_and_leaves_the_stored_one_alone(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    const object_store_geometry_t cs_before = cs;
    object_store_geometry_t out;
    size_t remotes = 0U;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, &remotes) == ESP_OK);
    assert(remotes == 4U);
    assert(out.anchored);
    for (unsigned i = 0U; i < 4U; ++i) {
        assert(out.pods[i].computed);
        assert(out.pods[i].x_mm == layout.nodes[i + 1U].x_mm && out.pods[i].y_mm == layout.nodes[i + 1U].y_mm && out.pods[i].z_mm == layout.nodes[i + 1U].z_mm);
        assert(out.pods[i].uncertainty_mm == (int32_t)(4U + i + 1U));   /* the tag's, not the Channel Sounding 150 */
        assert(out.pods[i].residual_mm == 0);
        assert(out.pods[i].leveled == cs.pods[i].leveled);              /* the sensor's own state */
        assert(out.pods[i].computed_at_ms == GEOMETRY_AT_MS + 3U);      /* the geometry's identity: the newest of the stored layout */
    }
    assert(equal(&cs, &cs_before));                                     /* BLE / barometer values were only read */
}

static void a_preliminary_tag_is_used_like_a_confirmed_one(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    assert(layout.nodes[2].confirmed == false && layout.nodes[1].confirmed == true);
    const seismic_recorder_session_participation_t part = session(4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    object_store_geometry_t out;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_OK);
    assert(out.pods[1].computed && out.pods[1].x_mm == 0 && out.pods[1].y_mm == 800);
}

/* Issue #26: a layout is valid only for the frozen geometry generation it was built against. */
static void a_layout_for_another_geometry_epoch_is_not_placement_evidence(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    object_store_geometry_t out;
    seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_OK);
    layout.geometry_computed_at_ms = GEOMETRY_AT_MS + 2U;      /* sensors were re-measured: another geometry generation */
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_INVALID_STATE);
    layout.geometry_computed_at_ms = GEOMETRY_AT_MS + 4U;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_INVALID_STATE);
    layout.geometry_computed_at_ms = 0U;                       /* staged scan / older file: no epoch recorded, used as before (the phone could not name one) */
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_OK);
    layout.geometry_computed_at_ms = GEOMETRY_AT_MS + 3U;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_OK);

    /* The Channel Sounding layout recomputed later (new timestamps) invalidates the same layout again, and nothing was rewritten. */
    object_store_geometry_t moved = cs_geometry(4U);
    for (unsigned i = 0U; i < 4U; ++i) moved.pods[i].computed_at_ms += 100U;
    const object_store_geometry_t moved_before = moved;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &moved, &out, NULL) == ESP_ERR_INVALID_STATE);
    assert(equal(&moved, &moved_before));
}

/* Issue #35: zero-evidence, zero-uncertainty or flat layouts are not eligible geometry (a plain preliminary tag with real sightings still is). */
static void unobserved_or_degenerate_layouts_are_not_geometry(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    object_store_geometry_t out;

    seismic_recorder_tag_layout_t flat = layout_of(all, 4U);
    flat.weakest_main_triangle_height_mm = SEISMIC_RECORDER_TAG_LAYOUT_MIN_TRIANGLE_HEIGHT_MM - 1;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &flat, &cs, &out, NULL) == ESP_ERR_NOT_FOUND);
    flat.weakest_main_triangle_height_mm = SEISMIC_RECORDER_TAG_LAYOUT_MIN_TRIANGLE_HEIGHT_MM;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &flat, &cs, &out, NULL) == ESP_OK);

    seismic_recorder_tag_layout_t unseen = layout_of(all, 4U);
    for (unsigned node = 1U; node <= 4U; ++node) { unseen.nodes[node].observations = 0U; unseen.nodes[node].views = 0U; }
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &unseen, &cs, &out, NULL) == ESP_ERR_NOT_FOUND);

    seismic_recorder_tag_layout_t no_views = layout_of(all, 4U);
    no_views.nodes[3].views = 0U;                       /* the unobserved remote drops out; three tagged cannot cover the four Channel Sounding pods */
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &no_views, &cs, &out, NULL) == ESP_ERR_INVALID_SIZE);
    seismic_recorder_tag_layout_t exact = layout_of(all, 4U);
    exact.nodes[3].uncertainty_mm = 0;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &exact, &cs, &out, NULL) == ESP_ERR_INVALID_SIZE);
}

static void the_triangle_is_judged_by_the_coordinates_not_by_the_files_claim(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    object_store_geometry_t out;

    /* Every remote on one line through MAIN: the file still CLAIMS a healthy 400 mm triangle. */
    seismic_recorder_tag_layout_t line = layout_of(all, 4U);
    for (unsigned node = 1U; node <= 4U; ++node) {
        line.nodes[node].x_mm = (int32_t)(node * 500);
        line.nodes[node].y_mm = 0;
        line.nodes[node].z_mm = 0;
    }
    assert(line.weakest_main_triangle_height_mm == 400);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &line, &cs, &out, NULL) == ESP_ERR_NOT_FOUND);

    /* Nearly on a line, three remotes: the best triangle (MAIN, 2, 3) is 29 mm high, one millimetre under the gate; 31 mm is accepted. */
    const unsigned three[] = {1U, 2U, 3U};
    const seismic_recorder_session_participation_t part3 = session(3U);
    const object_store_geometry_t cs3 = cs_geometry(3U);
    seismic_recorder_tag_layout_t almost = layout_of(three, 3U);
    almost.nodes[1].x_mm = 500;  almost.nodes[1].y_mm = 0;  almost.nodes[1].z_mm = 0;
    almost.nodes[2].x_mm = 1000; almost.nodes[2].y_mm = 0;  almost.nodes[2].z_mm = 0;
    almost.nodes[3].x_mm = 2000; almost.nodes[3].y_mm = 58; almost.nodes[3].z_mm = 0;
    assert(seismic_recorder_geometry_apply_tag_layout(&part3, &almost, &cs3, &out, NULL) == ESP_ERR_NOT_FOUND);
    almost.nodes[3].y_mm = 62;
    assert(seismic_recorder_geometry_apply_tag_layout(&part3, &almost, &cs3, &out, NULL) == ESP_OK);

    /* The honest layout of the other tests is untouched by the new check. */
    seismic_recorder_tag_layout_t good = layout_of(all, 4U);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &good, &cs, &out, NULL) == ESP_OK);
}

static void only_tagged_remotes_take_part_and_none_borrows_a_channel_sounding_position(void)
{
    const unsigned three[] = {1U, 2U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const seismic_recorder_tag_layout_t layout = layout_of(three, 3U);
    const object_store_geometry_t cs = cs_geometry(3U);               /* the Channel Sounding layout also covers three */
    object_store_geometry_t out;
    size_t remotes = 0U;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, &remotes) == ESP_OK);
    assert(remotes == 3U);
    assert(out.pods[0].computed && out.pods[1].computed && !out.pods[2].computed && out.pods[3].computed);
    assert(out.pods[2].x_mm == 0 && out.pods[2].y_mm == 0 && out.pods[2].z_mm == 0 && out.pods[2].uncertainty_mm == 0);
    assert(out.pods[2].leveled == cs.pods[2].leveled);
}

static void a_layout_that_covers_fewer_remotes_than_channel_sounding_is_not_used(void)
{
    const unsigned three[] = {1U, 2U, 3U};
    const seismic_recorder_session_participation_t part = session(4U);
    const seismic_recorder_tag_layout_t layout = layout_of(three, 3U);
    const object_store_geometry_t cs = cs_geometry(4U);               /* four remotes: 3D-capable; three would be planar only */
    object_store_geometry_t out;
    memset(&out, 0x5A, sizeof(out));
    object_store_geometry_t untouched = out;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_INVALID_SIZE);
    assert(equal(&out, &untouched));
}

static void a_layout_can_serve_when_channel_sounding_could_not_place_every_remote(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    const object_store_geometry_t cs = cs_geometry(2U);               /* ranging placed only two */
    object_store_geometry_t out;
    size_t remotes = 0U;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, &remotes) == ESP_OK);
    assert(remotes == 4U && out.pods[2].computed && out.pods[3].computed);
}

static void too_few_tagged_remotes_or_no_main_is_not_a_layout(void)
{
    const unsigned two[] = {1U, 2U};
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const object_store_geometry_t cs = cs_geometry(2U);
    object_store_geometry_t out;
    memset(&out, 0x5A, sizeof(out));
    const object_store_geometry_t untouched = out;

    seismic_recorder_tag_layout_t layout = layout_of(two, 2U);       /* MAIN + two remotes: a triangle, but a localization needs three remotes */
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_NOT_FOUND);
    layout = layout_of(all, 4U);
    layout.nodes[0].valid = false;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_NOT_FOUND);
    layout = layout_of(all, 4U);
    layout.available = false;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_NOT_FOUND);
    assert(equal(&out, &untouched));
}

static void a_tag_for_a_remote_that_is_not_this_sessions_is_ignored(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    seismic_recorder_session_participation_t part = session(4U);
    part.pods[2].assigned = false;                                    /* G3 was not part of the session */
    const seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    object_store_geometry_t cs = cs_geometry(4U);
    cs.pods[2].computed = false;
    object_store_geometry_t out;
    size_t remotes = 0U;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, &remotes) == ESP_OK);
    assert(remotes == 3U && !out.pods[2].computed);

    part = session(4U);
    part.pods[0].node_id = 7U;                                        /* the pod's node id is not the tag's node */
    cs = cs_geometry(4U);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, &remotes) == ESP_ERR_INVALID_SIZE);
    part.pods[0].node_id_known = false;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, &remotes) == ESP_ERR_INVALID_SIZE);
}

static void two_sensors_in_one_place_is_a_tag_mix_up_not_a_layout(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    object_store_geometry_t out;
    memset(&out, 0x5A, sizeof(out));
    const object_store_geometry_t untouched = out;

    seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    layout.nodes[2].x_mm = layout.nodes[1].x_mm + 3; layout.nodes[2].y_mm = layout.nodes[1].y_mm; layout.nodes[2].z_mm = layout.nodes[1].z_mm;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_INVALID_ARG);
    layout = layout_of(all, 4U);
    layout.nodes[3].x_mm = 4; layout.nodes[3].y_mm = 4; layout.nodes[3].z_mm = 4;   /* on top of MAIN, the origin */
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_ERR_INVALID_ARG);
    assert(equal(&out, &untouched));

    layout = layout_of(all, 4U);
    layout.nodes[2].x_mm = layout.nodes[1].x_mm + 10; layout.nodes[2].y_mm = layout.nodes[1].y_mm; layout.nodes[2].z_mm = layout.nodes[1].z_mm;
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, &out, NULL) == ESP_OK);  /* exactly 10 mm apart is allowed */
}

static void null_arguments_are_refused(void)
{
    const unsigned all[] = {1U, 2U, 3U, 4U};
    const seismic_recorder_session_participation_t part = session(4U);
    const seismic_recorder_tag_layout_t layout = layout_of(all, 4U);
    const object_store_geometry_t cs = cs_geometry(4U);
    object_store_geometry_t out;
    size_t remotes = 99U;
    assert(seismic_recorder_geometry_apply_tag_layout(NULL, &layout, &cs, &out, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, NULL, &cs, &out, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, NULL, &out, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_apply_tag_layout(&part, &layout, &cs, NULL, &remotes) == ESP_ERR_INVALID_ARG);
    assert(remotes == 99U);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"a_full_layout_becomes_the_geometry_and_leaves_the_stored_one_alone", a_full_layout_becomes_the_geometry_and_leaves_the_stored_one_alone},
        {"a_preliminary_tag_is_used_like_a_confirmed_one", a_preliminary_tag_is_used_like_a_confirmed_one},
        {"unobserved_or_degenerate_layouts_are_not_geometry", unobserved_or_degenerate_layouts_are_not_geometry},
        {"the_triangle_is_judged_by_the_coordinates_not_by_the_files_claim", the_triangle_is_judged_by_the_coordinates_not_by_the_files_claim},
        {"a_layout_for_another_geometry_epoch_is_not_placement_evidence", a_layout_for_another_geometry_epoch_is_not_placement_evidence},
        {"only_tagged_remotes_take_part_and_none_borrows_a_channel_sounding_position", only_tagged_remotes_take_part_and_none_borrows_a_channel_sounding_position},
        {"a_layout_that_covers_fewer_remotes_than_channel_sounding_is_not_used", a_layout_that_covers_fewer_remotes_than_channel_sounding_is_not_used},
        {"a_layout_can_serve_when_channel_sounding_could_not_place_every_remote", a_layout_can_serve_when_channel_sounding_could_not_place_every_remote},
        {"too_few_tagged_remotes_or_no_main_is_not_a_layout", too_few_tagged_remotes_or_no_main_is_not_a_layout},
        {"a_tag_for_a_remote_that_is_not_this_sessions_is_ignored", a_tag_for_a_remote_that_is_not_this_sessions_is_ignored},
        {"two_sensors_in_one_place_is_a_tag_mix_up_not_a_layout", two_sensors_in_one_place_is_a_tag_mix_up_not_a_layout},
        {"null_arguments_are_refused", null_arguments_are_refused},
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
