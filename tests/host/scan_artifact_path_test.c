#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#else
#define MAKE_DIR(path) mkdir(path, 0777)
#endif

#include "scan_artifact_path.h"

#define ROOT "scan_artifact_path_tmp"

static void dir(const char *path) { (void)MAKE_DIR(path); }

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fwrite(text, 1U, strlen(text), file) == strlen(text) && fclose(file) == 0);
}

static void layout(void)
{
    dir(ROOT);
    dir(ROOT "/OBJ_0001");
    dir(ROOT "/OBJ_0001/sessions");
    dir(ROOT "/OBJ_0001/sessions/S000001");
    dir(ROOT "/OBJ_0001/sessions/S000002");
    dir(ROOT "/OBJ_0001/scans");
    dir(ROOT "/OBJ_0001/scans/SC000001");
}

static void clean(void)
{
    static const char *files[] = {
        ROOT "/OBJ_0001/sessions/S000001/scan-v1.bin", ROOT "/OBJ_0001/sessions/S000001/scan-ref",
        ROOT "/OBJ_0001/sessions/S000002/scan-v1.bin", ROOT "/OBJ_0001/sessions/S000002/scan-ref",
        ROOT "/OBJ_0001/sessions/S000002/occupancy-v1.bin", ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin",
        ROOT "/OBJ_0001/scans/SC000001/occupancy-v1.bin",
    };
    for (size_t i = 0U; i < sizeof(files) / sizeof(files[0]); ++i) (void)remove(files[i]);
}

static void resolve(const char *session, const char *file, char out[256])
{
    assert(scan_artifact_resolve(ROOT, "OBJ_0001", session, file, out, 256U) == 0);
}

static void test_ids(void)
{
    assert(scan_artifact_id_valid("SC000001"));
    assert(!scan_artifact_id_valid("SC00001") && !scan_artifact_id_valid("S0000001") && !scan_artifact_id_valid("SC00000a"));
    assert(!scan_artifact_id_valid("SC0000011") && !scan_artifact_id_valid("") && !scan_artifact_id_valid(NULL));
}

static void test_a_session_follows_its_reference_to_the_shared_scan(void)
{
    clean();
    write_file(ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin", "shared scan");
    write_file(ROOT "/OBJ_0001/sessions/S000002/scan-ref", "SC000001\n");
    char path[256];
    resolve("S000002", "scan-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin") == 0);
    /* A session without a reference has nothing to follow. */
    resolve("S000001", "scan-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/sessions/S000001/scan-v1.bin") == 0);
}

static void test_the_sessions_own_file_wins(void)
{
    clean();
    write_file(ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin", "shared scan");
    write_file(ROOT "/OBJ_0001/scans/SC000001/occupancy-v1.bin", "shared occupancy");
    write_file(ROOT "/OBJ_0001/sessions/S000002/scan-ref", "SC000001");
    write_file(ROOT "/OBJ_0001/sessions/S000002/occupancy-v1.bin", "this session's own occupancy");
    write_file(ROOT "/OBJ_0001/sessions/S000001/scan-v1.bin", "old session, own copy");
    char path[256];
    resolve("S000002", "occupancy-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/sessions/S000002/occupancy-v1.bin") == 0);
    resolve("S000002", "scan-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin") == 0);
    resolve("S000001", "scan-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/sessions/S000001/scan-v1.bin") == 0);
}

static void test_a_reference_that_leads_nowhere_reports_not_found_as_before(void)
{
    clean();
    char path[256];
    write_file(ROOT "/OBJ_0001/sessions/S000002/scan-ref", "SC000009");
    resolve("S000002", "scan-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/sessions/S000002/scan-v1.bin") == 0);
    /* The file the reference points at is missing in the shared folder. */
    write_file(ROOT "/OBJ_0001/sessions/S000002/scan-ref", "SC000001");
    resolve("S000002", "occupancy-v1.bin", path);
    assert(strcmp(path, ROOT "/OBJ_0001/sessions/S000002/occupancy-v1.bin") == 0);
    /* A reference that is not a scan id (a path, a longer name) is ignored: it must never reach outside scans/. */
    write_file(ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin", "shared scan");
    const char *bad[] = {"../S000001", "SC0000011", "sc000001", "SC00000", "", "SC000001/../..", "SC00000\xff"};
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        write_file(ROOT "/OBJ_0001/sessions/S000002/scan-ref", bad[i]);
        resolve("S000002", "scan-v1.bin", path);
        assert(strcmp(path, ROOT "/OBJ_0001/sessions/S000002/scan-v1.bin") == 0);
    }
}

static void test_paths_that_do_not_fit_are_refused(void)
{
    char tiny[24];
    assert(scan_artifact_resolve(ROOT, "OBJ_0001", "S000002", "scan-v1.bin", tiny, sizeof(tiny)) == -1);
    clean();
    write_file(ROOT "/OBJ_0001/scans/SC000001/scan-v1.bin", "x");
    write_file(ROOT "/OBJ_0001/sessions/S000002/scan-ref", "SC000001");
    char fits_session_only[60];
    const int result = scan_artifact_resolve(ROOT, "OBJ_0001", "S000002", "scan-v1.bin", fits_session_only, sizeof(fits_session_only));
    /* Either answer is fine, but never a truncated path. */
    assert(result == 0 ? strstr(fits_session_only, "scan-v1.bin") != NULL : result == -1);
}

int main(void)
{
    layout();
    test_ids();
    test_a_session_follows_its_reference_to_the_shared_scan();
    test_the_sessions_own_file_wins();
    test_a_reference_that_leads_nowhere_reports_not_found_as_before();
    test_paths_that_do_not_fit_are_refused();
    clean();
    puts("scan_artifact_path_test: OK");
    return 0;
}
