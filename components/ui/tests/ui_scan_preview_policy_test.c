#include "ui_scan_preview_policy.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* MAX-415/MAX-414: scan identity and run eligibility for the TFT scan preview. */

#define SCAN_A 0x11111111U
#define SCAN_B 0x0000BEEFU
#define EMPTY_TEXT "\0"

static void identity_of(uint32_t crc, char out[UI_SCAN_PREVIEW_IDENTITY_SIZE])
{
    const bool formatted = ui_scan_preview_format_identity(crc, out, UI_SCAN_PREVIEW_IDENTITY_SIZE);
    assert(formatted);
    (void)formatted;
}

static void test_identity_format(void)
{
    char text[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    char small[UI_SCAN_PREVIEW_IDENTITY_SIZE - 1U];

    identity_of(0xDEADBEEFU, text);
    assert(strcmp(text, "scan-crc32:deadbeef") == 0);
    identity_of(0x0000ABCDU, text);
    assert(strcmp(text, "scan-crc32:0000abcd") == 0);
    identity_of(0U, text);
    assert(strcmp(text, "scan-crc32:00000000") == 0);

    memset(small, 120, sizeof(small));
    assert(!ui_scan_preview_format_identity(SCAN_A, small, sizeof(small)));
    assert(small[0] == 0);
    assert(!ui_scan_preview_format_identity(SCAN_A, NULL, UI_SCAN_PREVIEW_IDENTITY_SIZE));
    assert(!ui_scan_preview_format_identity(SCAN_A, text, 0U));
}

static void test_occupancy_frame_id_must_match_current_scan(void)
{
    char a[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    char b[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    identity_of(SCAN_A, a);
    identity_of(SCAN_B, b);

    /* B/B: the occupancy fallback of the current scan is accepted. */
    assert(ui_scan_preview_occupancy_matches_scan(b, SCAN_B));
    /* Occupancy of scan A against the replaced scan B is stale, and vice versa. */
    assert(!ui_scan_preview_occupancy_matches_scan(a, SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan(b, SCAN_A));

    assert(!ui_scan_preview_occupancy_matches_scan(NULL, SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan(EMPTY_TEXT, SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan("scan-crc32:0000BEEF", SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan("scan-crc32:0000beef ", SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan("scan-crc32:0000beef0", SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan("scan-crc32:0000bee", SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan("crc32:0000beef", SCAN_B));
    assert(!ui_scan_preview_occupancy_matches_scan("0000beef", SCAN_B));
}

static void test_method_classification_table(void)
{
    static const char *const constrained[] = {
        "traveltime_scan_constrained", "semblance_scan_constrained", "tdcas_scan_constrained",
        "psafuvs_scan_constrained", "suea_scan_voids", "eikonal_scan_voids",
        "semblance_eikonal_voids", "bayesian_scan_voids", "_voids",
    };
    static const char *const unconstrained[] = {
        "traveltime", "semblance", "tdcas", "psafuvs", "suea", "eikonal_fmm", "bayesian",
        "xcorr_tdoa", "gauss_newton_tdoa", "joint_fusion", "localization_analysis",
        "scan_constrained", "voids", "semblance_voids_extra", EMPTY_TEXT,
    };
    for (size_t i = 0U; i < sizeof(constrained) / sizeof(constrained[0]); ++i) {
        assert(ui_scan_preview_method_is_scan_constrained(constrained[i]));
    }
    for (size_t i = 0U; i < sizeof(unconstrained) / sizeof(unconstrained[0]); ++i) {
        assert(!ui_scan_preview_method_is_scan_constrained(unconstrained[i]));
    }
    assert(!ui_scan_preview_method_is_scan_constrained(NULL));
}

static void test_replaced_scan_run_eligibility(void)
{
    static const char *const names[] = {
        "semblance_scan_constrained", "tdcas_scan_constrained", "psafuvs_scan_constrained",
        "suea_scan_voids", "eikonal_scan_voids", "semblance_eikonal_voids", "bayesian_scan_voids",
    };
    char a[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    char b[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    identity_of(SCAN_A, a);
    identity_of(SCAN_B, b);

    /* The scan was replaced: A is gone, B is current. */
    ui_scan_preview_run_binding_t run = {
        .method_name = "traveltime_scan_constrained",
        .scan_provenance_present = true,
        .scan_constraint_applied = true,
        .scan_frame_id = a,
    };
    assert(ui_scan_preview_run_eligibility(&run, b) == UI_SCAN_PREVIEW_RUN_STALE_SCAN);

    /* A run bound to B is current. */
    run.scan_frame_id = b;
    assert(ui_scan_preview_run_eligibility(&run, b) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
    /* The same run is history once the current scan is A again. */
    assert(ui_scan_preview_run_eligibility(&run, a) == UI_SCAN_PREVIEW_RUN_STALE_SCAN);

    /* Every constrained method name is gated the same way. */
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        run.method_name = names[i];
        run.scan_frame_id = a;
        assert(ui_scan_preview_run_eligibility(&run, b) == UI_SCAN_PREVIEW_RUN_STALE_SCAN);
        run.scan_frame_id = b;
        assert(ui_scan_preview_run_eligibility(&run, b) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
    }

    /* Unconstrained runs follow the geometry rule only, never the scan identity. */
    const ui_scan_preview_run_binding_t unconstrained_legacy = {.method_name = "semblance"};
    assert(ui_scan_preview_run_eligibility(&unconstrained_legacy, b) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
    assert(ui_scan_preview_run_eligibility(&unconstrained_legacy, NULL) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
    const ui_scan_preview_run_binding_t unconstrained_v6 = {
        .method_name = "semblance", .scan_provenance_present = true,
        .scan_constraint_applied = false, .scan_frame_id = NULL,
    };
    assert(ui_scan_preview_run_eligibility(&unconstrained_v6, b) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
}

static void test_legacy_and_unverifiable_constrained_runs_are_excluded(void)
{
    char b[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    identity_of(SCAN_B, b);

    /* Schema below 6: scan generation unknown, even if a frame id string happens to match. */
    const ui_scan_preview_run_binding_t legacy = {
        .method_name = "tdcas_scan_constrained", .scan_provenance_present = false,
        .scan_constraint_applied = true, .scan_frame_id = b,
    };
    assert(ui_scan_preview_run_eligibility(&legacy, b) == UI_SCAN_PREVIEW_RUN_LEGACY_CONSTRAINED);

    /* Constrained-named but the recorded provenance says no constraint was applied. */
    const ui_scan_preview_run_binding_t not_applied = {
        .method_name = "suea_scan_voids", .scan_provenance_present = true,
        .scan_constraint_applied = false, .scan_frame_id = NULL,
    };
    assert(ui_scan_preview_run_eligibility(&not_applied, b) == UI_SCAN_PREVIEW_RUN_CONSTRAINT_NOT_APPLIED);

    /* Current scan identity could not be established: fail closed. */
    ui_scan_preview_run_binding_t bound = {
        .method_name = "tdcas_scan_constrained", .scan_provenance_present = true,
        .scan_constraint_applied = true, .scan_frame_id = b,
    };
    assert(ui_scan_preview_run_eligibility(&bound, NULL) == UI_SCAN_PREVIEW_RUN_SCAN_UNKNOWN);
    assert(ui_scan_preview_run_eligibility(&bound, EMPTY_TEXT) == UI_SCAN_PREVIEW_RUN_SCAN_UNKNOWN);

    /* A missing or empty recorded frame id never matches. */
    bound.scan_frame_id = NULL;
    assert(ui_scan_preview_run_eligibility(&bound, b) == UI_SCAN_PREVIEW_RUN_STALE_SCAN);
    bound.scan_frame_id = EMPTY_TEXT;
    assert(ui_scan_preview_run_eligibility(&bound, b) == UI_SCAN_PREVIEW_RUN_STALE_SCAN);

    assert(ui_scan_preview_run_eligibility(NULL, b) == UI_SCAN_PREVIEW_RUN_SCAN_UNKNOWN);
}

static void test_joint_fusion_follows_its_own_provenance(void)
{
    char a[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    char b[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    identity_of(SCAN_A, a);
    identity_of(SCAN_B, b);

    ui_scan_preview_run_binding_t fusion = {
        .method_name = "joint_fusion", .scan_provenance_present = true,
        .scan_constraint_applied = true, .scan_frame_id = a,
    };
    assert(ui_scan_preview_run_eligibility(&fusion, b) == UI_SCAN_PREVIEW_RUN_STALE_SCAN);
    fusion.scan_frame_id = b;
    assert(ui_scan_preview_run_eligibility(&fusion, b) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
    fusion.scan_constraint_applied = false;
    fusion.scan_frame_id = NULL;
    assert(ui_scan_preview_run_eligibility(&fusion, b) == UI_SCAN_PREVIEW_RUN_ELIGIBLE);
}

static void test_scan_and_sensor_estimate_view_selection(void)
{
    assert(ui_scan_preview_select_view(false, false, 0U, 0U) == UI_SCAN_PREVIEW_VIEW_UNAVAILABLE);
    assert(ui_scan_preview_select_view(false, false, 2U, 0U) == UI_SCAN_PREVIEW_VIEW_SENSOR_ESTIMATES);
    assert(ui_scan_preview_select_view(false, false, 0U, 1U) == UI_SCAN_PREVIEW_VIEW_SENSOR_ESTIMATES);
    assert(ui_scan_preview_select_view(true, true, 2U, 17U) == UI_SCAN_PREVIEW_VIEW_SCAN);
    assert(ui_scan_preview_select_view(true, false, 2U, 17U) == UI_SCAN_PREVIEW_VIEW_UNAVAILABLE);
}

static void test_empty_session_is_the_staged_scan(void)
{
    assert(ui_scan_preview_session_is_staged(""));
    assert(ui_scan_preview_session_is_staged(NULL));
    assert(!ui_scan_preview_session_is_staged("S000001"));
    assert(!ui_scan_preview_session_is_staged("staging"));   /* the web token is not a session directory on the card */
    assert(ui_scan_preview_request_ids_valid("OBJ_0001", 32U, "", 16U));
    assert(ui_scan_preview_request_ids_valid("OBJ_0001", 32U, NULL, 16U));
    assert(ui_scan_preview_request_ids_valid("OBJ_0001", 32U, "S000001", 16U));
    assert(!ui_scan_preview_request_ids_valid("", 32U, "S000001", 16U));
    assert(!ui_scan_preview_request_ids_valid(NULL, 32U, "", 16U));
    assert(!ui_scan_preview_request_ids_valid("OBJ/../x", 32U, "", 16U));
    assert(!ui_scan_preview_request_ids_valid("OBJ_0001", 32U, "S0/../1", 16U));
    assert(!ui_scan_preview_request_ids_valid("OBJ_0001", 4U, "", 16U));          /* does not fit its buffer */
    assert(!ui_scan_preview_id_is_safe("a b", 16U));
    assert(!ui_scan_preview_id_is_safe("", 16U));
    assert(ui_scan_preview_id_is_safe("A_b-9", 16U));
}

static void test_staged_path_is_in_the_staging_directory(void)
{
    char path[64];
    assert(ui_scan_preview_staged_path("/sdcard/objects", "OBJ_0001", "preview-v1.bin", path, sizeof(path)));
    assert(strcmp(path, "/sdcard/objects/OBJ_0001/staging/preview-v1.bin") == 0);
    assert(!strstr(path, "sessions"));
    char tiny[16];
    memset(tiny, 'x', sizeof(tiny));
    assert(!ui_scan_preview_staged_path("/sdcard/objects", "OBJ_0001", "scan-v1.bin", tiny, sizeof(tiny)));
    assert(tiny[0] == '\0');
    assert(!ui_scan_preview_staged_path(NULL, "OBJ_0001", "scan-v1.bin", path, sizeof(path)) && path[0] == '\0');
    assert(!ui_scan_preview_staged_path("/sdcard/objects", "OBJ_0001", "", path, sizeof(path)));
    assert(!ui_scan_preview_staged_path("/sdcard/objects", "OBJ_0001", "scan-v1.bin", NULL, 8U));
}

int main(void)
{
    test_empty_session_is_the_staged_scan();
    test_staged_path_is_in_the_staging_directory();
    test_identity_format();
    test_scan_and_sensor_estimate_view_selection();
    test_occupancy_frame_id_must_match_current_scan();
    test_method_classification_table();
    test_replaced_scan_run_eligibility();
    test_legacy_and_unverifiable_constrained_runs_are_excluded();
    test_joint_fusion_follows_its_own_provenance();
    return 0;
}
