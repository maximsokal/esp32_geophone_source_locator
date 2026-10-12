#ifdef _WIN32
#include <process.h>
#define MAX371_HARNESS_CHDIR _chdir
#define MAX371_HARNESS_GETPID _getpid
#else
#include <unistd.h>
#define MAX371_HARNESS_CHDIR chdir
#define MAX371_HARNESS_GETPID getpid
#endif

#define main max371_recorder_samples_contract_main
#include "../../components/seismic_recorder/tests/seismic_recorder_fs_contract_test.c"
#undef main

static int max371_create_unique_directory(char *directory, size_t capacity, const char *prefix)
{
    const unsigned long process_id = (unsigned long)MAX371_HARNESS_GETPID();
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        const int length = snprintf(directory, capacity, "%s-%lu-%u", prefix, process_id, attempt);
        if (length < 0 || (size_t)length >= capacity) {
            return -1;
        }
        if (mkdir(directory, 0775) == 0) {
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    return -1;
}

static void test_previous_run_staging_file_is_isolated(
    const char *run_directory,
    const char *previous_directory)
{
    char previous_file_from_run[256];
    const int path_length = snprintf(
        previous_file_from_run,
        sizeof(previous_file_from_run),
        "../%s/%s/geolocator/objects/OBJ_0001/staging/preview-v1.bin",
        previous_directory,
        STORAGE_MANAGER_MOUNT_POINT
    );
    assert(path_length > 0 && (size_t)path_length < sizeof(previous_file_from_run));

    assert(MAX371_HARNESS_CHDIR(previous_directory) == 0);
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging") == 0);
    FILE *stale_file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/preview-v1.bin", "wb");
    assert(stale_file != NULL);
    static const char stale_contents[] = "previous-run";
    assert(fwrite(stale_contents, 1U, sizeof(stale_contents) - 1U, stale_file) == sizeof(stale_contents) - 1U);
    assert(fclose(stale_file) == 0);

    assert(MAX371_HARNESS_CHDIR("..") == 0);
    assert(MAX371_HARNESS_CHDIR(run_directory) == 0);
    remove_layout();
    test_staged_artifacts_attach_once_to_the_created_session();
    assert_file_contents(previous_file_from_run, stale_contents);
    remove_layout();

    assert(MAX371_HARNESS_CHDIR("..") == 0);
    assert(MAX371_HARNESS_CHDIR(previous_directory) == 0);
    remove_layout();
    assert(MAX371_HARNESS_CHDIR("..") == 0);
    assert(rmdir(previous_directory) == 0);
    assert(MAX371_HARNESS_CHDIR(run_directory) == 0);
}

int main(void)
{
    char run_directory[96];
    char previous_directory[96];

    /* The contract test uses a fixed relative mount path. Shared CWDs let parallel runs delete or reuse each other's staging and session files. */
    if (max371_create_unique_directory(run_directory, sizeof(run_directory), "run") != 0) {
        return 2;
    }
    if (max371_create_unique_directory(previous_directory, sizeof(previous_directory), "previous") != 0) {
        assert(MAX371_HARNESS_CHDIR("..") == 0);
        (void)rmdir(run_directory);
        return 2;
    }

    test_previous_run_staging_file_is_isolated(run_directory, previous_directory);
    const int result = max371_recorder_samples_contract_main();
    remove_layout();
    assert(MAX371_HARNESS_CHDIR("..") == 0);
    assert(rmdir(run_directory) == 0);
    return result;
}
