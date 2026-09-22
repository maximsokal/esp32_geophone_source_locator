#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "localization_run_store.h"


static int s_write_call_count;
static char s_last_path[512];
static char s_last_text[4096];
static esp_err_t s_next_write_result = ESP_OK;

esp_err_t storage_manager_read_text(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    (void)path;
    (void)buffer;
    (void)buffer_size;
    return ESP_ERR_NOT_FOUND;
}


esp_err_t storage_manager_atomic_write_text(
    const char *path,
    const char *text
)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_write_call_count;
    snprintf(s_last_path, sizeof(s_last_path), "%s", path);
    snprintf(s_last_text, sizeof(s_last_text), "%s", text);
    return s_next_write_result;
}


static localization_run_record_t solved_record(void)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));

    snprintf(record.object_id, sizeof(record.object_id), "OBJ_0007");
    snprintf(record.session_id, sizeof(record.session_id), "S000003");
    record.computed_at_ms = 123456789ULL;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(record.method_name, sizeof(record.method_name), "tdoa_gauss_newton");
    record.method_version = 1U;
    record.channel_mask_used = 0x0Fu;
    record.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    record.propagation_speed_mps = 340.0;
    record.solved = true;
    record.source_position = (localization_point3d_t) { 1.25, 0.5, 2.0 };
    record.rms_residual_us = 0.42;
    record.converged = true;

    return record;
}


static void reset_fake(void)
{
    s_write_call_count = 0;
    s_last_path[0] = '\0';
    s_last_text[0] = '\0';
    s_next_write_result = ESP_OK;
}


static void test_build_path_follows_the_project_session_layout(void)
{
    const localization_run_record_t record = solved_record();
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];

    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S000003", &record, path, sizeof(path)
        ) == ESP_OK
    );
    assert(
        strcmp(
            path,
            "/sdcard/geolocator/objects/OBJ_0007/sessions/S000003/"
            "localization_run_individual_tdoa_gauss_newton_123456789.json"
        ) == 0
    );
}


static void test_joint_kind_uses_a_different_slug(void)
{
    localization_run_record_t record = solved_record();
    record.kind = LOCALIZATION_RUN_KIND_JOINT_FUSION;
    snprintf(record.method_name, sizeof(record.method_name), "joint_fusion");
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];

    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S000003", &record, path, sizeof(path)
        ) == ESP_OK
    );
    assert(strstr(path, "localization_run_joint_joint_fusion_") != NULL);
}


static void test_different_runs_never_share_a_path(void)
{
    const localization_run_record_t a = solved_record();

    localization_run_record_t b = a;
    b.computed_at_ms = a.computed_at_ms + 1u;

    localization_run_record_t c = a;
    snprintf(c.method_name, sizeof(c.method_name), "traveltime_inversion");

    char path_a[LOCALIZATION_RUN_STORE_PATH_MAX];
    char path_b[LOCALIZATION_RUN_STORE_PATH_MAX];
    char path_c[LOCALIZATION_RUN_STORE_PATH_MAX];

    assert(
        localization_run_store_build_path("OBJ_0007", "S000003", &a, path_a, sizeof(path_a)) ==
        ESP_OK
    );
    assert(
        localization_run_store_build_path("OBJ_0007", "S000003", &b, path_b, sizeof(path_b)) ==
        ESP_OK
    );
    assert(
        localization_run_store_build_path("OBJ_0007", "S000003", &c, path_c, sizeof(path_c)) ==
        ESP_OK
    );

    assert(strcmp(path_a, path_b) != 0);
    assert(strcmp(path_a, path_c) != 0);
}


static void test_write_round_trips_through_the_fake_atomic_writer(void)
{
    reset_fake();
    const localization_run_record_t original = solved_record();

    assert(localization_run_store_write("OBJ_0007", "S000003", &original) == ESP_OK);
    assert(s_write_call_count == 1);
    assert(strstr(s_last_path, "/sessions/S000003/localization_run_") != NULL);

    localization_run_record_t parsed;
    assert(localization_run_record_from_json(s_last_text, &parsed) == ESP_OK);
    assert(strcmp(parsed.object_id, original.object_id) == 0);
    assert(strcmp(parsed.method_name, original.method_name) == 0);
    assert(parsed.solved);
    assert(parsed.source_position.x_m == original.source_position.x_m);
}


static void test_write_propagates_the_atomic_writers_own_error(void)
{
    reset_fake();
    s_next_write_result = ESP_ERR_NOT_FOUND;
    const localization_run_record_t record = solved_record();

    assert(localization_run_store_write("OBJ_0007", "S000003", &record) == ESP_ERR_NOT_FOUND);
    assert(s_write_call_count == 1);
}


static void test_write_never_calls_the_writer_for_an_invalid_record(void)
{
    reset_fake();
    localization_run_record_t record = solved_record();
    record.solved = false;
    record.rejection_reason[0] = '\0'; /* unsolved without a reason: to_json rejects this */

    assert(localization_run_store_write("OBJ_0007", "S000003", &record) == ESP_ERR_INVALID_ARG);
    assert(s_write_call_count == 0);
}


static void test_rejects_invalid_object_id(void)
{
    const localization_run_record_t record = solved_record();
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];

    assert(
        localization_run_store_build_path(
            "not-an-object-id", "S000003", &record, path, sizeof(path)
        ) == ESP_ERR_INVALID_ARG
    );
}


static void test_rejects_non_canonical_session_id(void)
{
    const localization_run_record_t record = solved_record();
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];

    assert(
        localization_run_store_build_path(
            "OBJ_0007", "session-3", &record, path, sizeof(path)
        ) == ESP_ERR_INVALID_ARG
    );
    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S3", &record, path, sizeof(path)
        ) == ESP_ERR_INVALID_ARG
    );
}


static void test_rejects_unsafe_method_name(void)
{
    /* method_name flows directly into the filename -- this is the one
     * path-traversal-relevant input this component controls. */
    localization_run_record_t record = solved_record();
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];

    snprintf(record.method_name, sizeof(record.method_name), "../../etc/passwd");
    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S000003", &record, path, sizeof(path)
        ) == ESP_ERR_INVALID_ARG
    );

    snprintf(record.method_name, sizeof(record.method_name), "tdoa gauss newton");
    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S000003", &record, path, sizeof(path)
        ) == ESP_ERR_INVALID_ARG
    );

    record.method_name[0] = '\0';
    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S000003", &record, path, sizeof(path)
        ) == ESP_ERR_INVALID_ARG
    );
}


static void test_rejects_a_path_buffer_too_small(void)
{
    const localization_run_record_t record = solved_record();
    char tiny_path[8];

    assert(
        localization_run_store_build_path(
            "OBJ_0007", "S000003", &record, tiny_path, sizeof(tiny_path)
        ) == ESP_ERR_INVALID_SIZE
    );
}


static void test_rejects_null_arguments(void)
{
    const localization_run_record_t record = solved_record();
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];

    assert(
        localization_run_store_build_path("OBJ_0007", "S000003", NULL, path, sizeof(path)) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_run_store_build_path("OBJ_0007", "S000003", &record, NULL, sizeof(path)) ==
        ESP_ERR_INVALID_ARG
    );
    assert(
        localization_run_store_write("OBJ_0007", "S000003", NULL) == ESP_ERR_INVALID_ARG
    );
}


int main(void)
{
    test_build_path_follows_the_project_session_layout();
    test_joint_kind_uses_a_different_slug();
    test_different_runs_never_share_a_path();
    test_write_round_trips_through_the_fake_atomic_writer();
    test_write_propagates_the_atomic_writers_own_error();
    test_write_never_calls_the_writer_for_an_invalid_record();
    test_rejects_invalid_object_id();
    test_rejects_non_canonical_session_id();
    test_rejects_unsafe_method_name();
    test_rejects_a_path_buffer_too_small();
    test_rejects_null_arguments();
    puts("localization run store tests passed");
    return 0;
}
