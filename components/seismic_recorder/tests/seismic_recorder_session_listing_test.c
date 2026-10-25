/*
 * Focused MAX-70 integration contract. Both production list translations are
 * included into one host binary so ordinary and responsive APIs must consume
 * the exact same bounded selector while preserving their different storage
 * arbitration policies.
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STORAGE_MANAGER_MOUNT_POINT "seismic_recorder_listing_tmp"

#include "object_format.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "seismic_recorder_internal.h"
#include "storage_manager.h"


static unsigned s_storage_begin_calls;
static unsigned s_storage_end_calls;
static unsigned s_known_present_reads;
static unsigned s_recovery_reads;
static esp_err_t s_summary_error = ESP_OK;


static int make_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}


bool object_format_id_is_valid(const char *object_id)
{
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}


esp_err_t object_store_get_profile(
    const char *object_id,
    object_store_profile_t *profile
)
{
    if (!object_format_id_is_valid(object_id) || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(profile, 0, sizeof(*profile));
    return ESP_OK;
}


esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->initialized = true;
    return ESP_OK;
}


esp_err_t storage_manager_operation_begin(void)
{
    ++s_storage_begin_calls;
    return ESP_OK;
}


void storage_manager_operation_end(void)
{
    ++s_storage_end_calls;
}


static esp_err_t fake_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary,
    bool known_present
)
{
    if (
        !object_format_id_is_valid(object_id) ||
        session_id == NULL ||
        summary == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (known_present) {
        ++s_known_present_reads;
    }
    else {
        ++s_recovery_reads;
    }

    memset(summary, 0, sizeof(*summary));
    if (s_summary_error != ESP_OK) {
        return s_summary_error;
    }
    snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
    summary->complete = true;
    summary->readable = true;
    return ESP_OK;
}


esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return fake_summary(object_id, session_id, summary, false);
}


esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return fake_summary(object_id, session_id, summary, true);
}


#include "../seismic_recorder_session_selection.c"
#include "../seismic_recorder_fs_list.c"
#include "../seismic_recorder_list_responsive.c"


static void make_layout(void)
{
    assert(make_directory(STORAGE_MANAGER_MOUNT_POINT) == 0);
    assert(make_directory(STORAGE_MANAGER_ROOT_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions") == 0);

    for (unsigned number = 1U; number <= 40U; ++number) {
        char session_path[512];
        char metadata_path[512];
        snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S%06u",
            number
        );
        assert(make_directory(session_path) == 0);
        snprintf(
            metadata_path,
            sizeof(metadata_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S%06u/session.json",
            number
        );
        FILE *metadata = fopen(metadata_path, "wb");
        assert(metadata != NULL);
        assert(fclose(metadata) == 0);
    }

    assert(make_directory(
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/not-a-session"
    ) == 0);
    assert(make_directory(
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S00000x"
    ) == 0);
}


static void remove_layout(void)
{
    char sessions_path[256];
    snprintf(
        sessions_path,
        sizeof(sessions_path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions"
    );

    DIR *sessions = opendir(sessions_path);
    if (sessions != NULL) {
        struct dirent *entry = NULL;
        while ((entry = readdir(sessions)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char session_path[768];
            char metadata_path[1024];
            snprintf(
                session_path,
                sizeof(session_path),
                "%s/%s",
                sessions_path,
                entry->d_name
            );
            snprintf(
                metadata_path,
                sizeof(metadata_path),
                "%s/session.json",
                session_path
            );
            (void)remove(metadata_path);
            (void)rmdir(session_path);
        }
        (void)closedir(sessions);
    }

    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}


static void reset_counters(void)
{
    s_storage_begin_calls = 0U;
    s_storage_end_calls = 0U;
    s_known_present_reads = 0U;
    s_recovery_reads = 0U;
}


static void assert_newest_32(
    const seismic_recorder_session_summary_t sessions[SEISMIC_RECORDER_MAX_SESSIONS]
)
{
    for (size_t index = 0U; index < SEISMIC_RECORDER_MAX_SESSIONS; ++index) {
        char expected[SEISMIC_RECORDER_SESSION_ID_SIZE];
        snprintf(
            expected,
            sizeof(expected),
            "S%06u",
            40U - (unsigned)index
        );
        assert(strcmp(sessions[index].session_id, expected) == 0);
    }
}


static void test_ordinary_and_responsive_share_newest_membership(void)
{
    seismic_recorder_session_summary_t ordinary[SEISMIC_RECORDER_MAX_SESSIONS] = {0};
    seismic_recorder_session_summary_t responsive[SEISMIC_RECORDER_MAX_SESSIONS] = {0};
    size_t ordinary_count = 0U;
    size_t responsive_count = 0U;

    reset_counters();
    assert(
        seismic_recorder_fs_list_sessions(
            "OBJ_0001",
            ordinary,
            100U,
            &ordinary_count
        ) == ESP_OK
    );
    assert(ordinary_count == SEISMIC_RECORDER_MAX_SESSIONS);
    assert_newest_32(ordinary);
    assert(s_known_present_reads == SEISMIC_RECORDER_MAX_SESSIONS);
    assert(s_recovery_reads == 0U);
    assert(s_storage_begin_calls == 1U);
    assert(s_storage_end_calls == 1U);

    reset_counters();
    assert(
        seismic_recorder_list_sessions_responsive(
            "OBJ_0001",
            responsive,
            100U,
            &responsive_count
        ) == ESP_OK
    );
    assert(responsive_count == SEISMIC_RECORDER_MAX_SESSIONS);
    assert_newest_32(responsive);
    assert(s_known_present_reads == SEISMIC_RECORDER_MAX_SESSIONS);
    assert(s_recovery_reads == 0U);
    assert(s_storage_begin_calls == 1U + SEISMIC_RECORDER_MAX_SESSIONS);
    assert(s_storage_end_calls == s_storage_begin_calls);

    for (size_t i = 0U; i < ordinary_count; ++i) {
        assert(strcmp(
            ordinary[i].session_id,
            responsive[i].session_id
        ) == 0);
    }
}


static void test_responsive_reads_metadata_only_for_retained_n(void)
{
    seismic_recorder_session_summary_t sessions[5] = {0};
    size_t count = 0U;

    reset_counters();
    assert(
        seismic_recorder_list_sessions_responsive(
            "OBJ_0001",
            sessions,
            5U,
            &count
        ) == ESP_OK
    );
    assert(count == 5U);
    assert(strcmp(sessions[0].session_id, "S000040") == 0);
    assert(strcmp(sessions[4].session_id, "S000036") == 0);
    assert(s_known_present_reads == 5U);
    assert(s_recovery_reads == 0U);
    assert(s_storage_begin_calls == 6U);
    assert(s_storage_end_calls == 6U);
}


static void test_summary_errors_are_classified_in_both_listings(void)
{
    seismic_recorder_session_summary_t sessions[3] = {0};
    size_t count = 0U;

    /* Malformed/absent metadata: the session is listed as incomplete. */
    s_summary_error = ESP_ERR_INVALID_RESPONSE;
    reset_counters();
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 3U, &count) == ESP_OK);
    assert(count == 3U);
    for (size_t i = 0U; i < count; ++i) {
        assert(!sessions[i].complete);
        assert(strcmp(sessions[i].session_id, i == 0U ? "S000040" : i == 1U ? "S000039" : "S000038") == 0);
    }
    count = 0U;
    reset_counters();
    assert(seismic_recorder_list_sessions_responsive("OBJ_0001", sessions, 3U, &count) == ESP_OK);
    assert(count == 3U);
    for (size_t i = 0U; i < count; ++i) {
        assert(!sessions[i].complete);
    }

    /* Out-of-memory / I/O failures are operational and must be reported. */
    const esp_err_t operational[] = {ESP_ERR_NO_MEM, ESP_FAIL};
    for (size_t e = 0U; e < sizeof(operational) / sizeof(operational[0]); ++e) {
        s_summary_error = operational[e];
        count = 0U;
        reset_counters();
        assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 3U, &count) == operational[e]);
        assert(s_storage_begin_calls == s_storage_end_calls);
        count = 0U;
        reset_counters();
        assert(seismic_recorder_list_sessions_responsive("OBJ_0001", sessions, 3U, &count) == operational[e]);
        assert(s_storage_begin_calls == s_storage_end_calls);
    }
    s_summary_error = ESP_OK;
}


int main(void)
{
    remove_layout();
    make_layout();
    test_summary_errors_are_classified_in_both_listings();
    test_ordinary_and_responsive_share_newest_membership();
    test_responsive_reads_metadata_only_for_retained_n();
    remove_layout();
    puts("SEISMIC_RECORDER_SESSION_LISTING_HOST_PASS");
    return 0;
}
