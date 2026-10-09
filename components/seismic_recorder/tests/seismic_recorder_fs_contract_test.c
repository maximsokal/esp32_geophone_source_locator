/*
 * This test includes the production filesystem translation unit so the host
 * binary exercises its real header/session/path code. Storage and object
 * boundaries are faked below; no user SD path is touched.
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(MAX371_RECORDER_RAW_ORACLE_HOST_TEST)
#include "ad7606_driver.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <unistd.h>
#endif

#define STORAGE_MANAGER_MOUNT_POINT "seismic_recorder_contract_tmp"

#include "seismic_recorder.h"
#include "seismic_buffer.h"
#include "object_store.h"
#include "storage_manager.h"
#include "seismic_recorder_cache.h"

/* The embedded libc declares these through its POSIX compatibility headers. */
#if !defined(_MSC_VER)
int fileno(FILE *stream);
size_t strnlen(const char *string, size_t max_length);
#endif

static unsigned storage_begin_calls;
static unsigned storage_end_calls;
static esp_err_t storage_begin_error;
static bool fail_create_directory;
static unsigned exclusive_directory_calls;
static unsigned remove_tree_calls;
static char removed_tree_path[256];
static esp_err_t fake_remove_tree_error;
static esp_err_t fake_summary_error = ESP_FAIL;
static bool fake_summary_override_active;
static seismic_recorder_session_summary_t fake_summary_override;
static size_t fake_summary_read_calls;
static size_t fake_known_present_summary_read_calls;
enum { RECORDER_TEST_WRITE_TRACE_CAPACITY = 64U };
static size_t recorder_test_fwrite_calls;
static size_t recorder_test_fwrite_requested_bytes[RECORDER_TEST_WRITE_TRACE_CAPACITY];
static size_t recorder_test_fwrite_accepted_bytes[RECORDER_TEST_WRITE_TRACE_CAPACITY];
static size_t recorder_test_fflush_calls;
static size_t recorder_test_fsync_calls;
static size_t recorder_test_fclose_calls;
static size_t injected_short_write_count = SIZE_MAX;
static size_t injected_short_write_call = SIZE_MAX;
static bool injected_fflush_error;
static bool injected_fsync_error;
static bool injected_fclose_error;
static char injected_rename_source[256];
static int seismic_recorder_contract_test_rename(const char *source, const char *destination);

#if defined(MAX371_RECORDER_RAW_ORACLE_HOST_TEST)
/* The oracle calls only the production driver's pure raw-to-uV function. */
esp_err_t gpio_config(const gpio_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    (void)gpio_num;
    (void)level;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return 0;
}

uint32_t esp_cpu_get_cycle_count(void)
{
    return 0U;
}

int esp_clk_cpu_freq(void)
{
    return 240000000;
}

void esp_rom_delay_us(uint32_t us)
{
    (void)us;
}

esp_err_t spi_bus_initialize(
    spi_host_device_t host_id,
    const spi_bus_config_t *bus_config,
    int dma_chan
)
{
    (void)host_id;
    (void)bus_config;
    (void)dma_chan;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(
    spi_host_device_t host_id,
    const spi_device_interface_config_t *config,
    spi_device_handle_t *handle
)
{
    (void)host_id;
    (void)config;
    if (handle != NULL) {
        *handle = NULL;
    }
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    (void)host_id;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(
    spi_device_handle_t handle,
    spi_transaction_t *transaction
)
{
    (void)handle;
    (void)transaction;
    return ESP_OK;
}

int spi_get_freq_limit(bool gpio_is_used, int input_delay_ns)
{
    (void)gpio_is_used;
    (void)input_delay_ns;
    return 12000000;
}
#endif


typedef enum
{
    SEISMIC_RECORDER_DIRECTORY_FAULT_NONE = 0,
    SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR,
    SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR,
    SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR,

} seismic_recorder_directory_fault_t;

static seismic_recorder_directory_fault_t directory_fault;
static unsigned directory_fault_call;
static int directory_fault_errno;
static unsigned directory_opendir_calls;
static unsigned directory_readdir_calls;
static unsigned directory_closedir_calls;

static void reset_directory_fault(void)
{
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_NONE;
    directory_fault_call = 0U;
    directory_fault_errno = 0;
    directory_opendir_calls = 0U;
    directory_readdir_calls = 0U;
    directory_closedir_calls = 0U;
}

static DIR *seismic_recorder_test_opendir(const char *path)
{
    ++directory_opendir_calls;
    if (
        directory_fault == SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR &&
        directory_opendir_calls == directory_fault_call
    ) {
        errno = directory_fault_errno;
        return NULL;
    }

    return opendir(path);
}

static struct dirent *seismic_recorder_test_readdir(DIR *directory)
{
    ++directory_readdir_calls;
    if (
        directory_fault == SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR &&
        directory_readdir_calls == directory_fault_call
    ) {
        errno = directory_fault_errno;
        return NULL;
    }

    return readdir(directory);
}

static int seismic_recorder_test_closedir(DIR *directory)
{
    ++directory_closedir_calls;
    if (
        directory_fault == SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR &&
        directory_closedir_calls == directory_fault_call
    ) {
        const int result = closedir(directory);
        errno = directory_fault_errno;
        return result == 0 ? -1 : result;
    }

    return closedir(directory);
}

static int make_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static void make_layout(void)
{
    assert(make_directory(STORAGE_MANAGER_MOUNT_POINT) == 0);
    assert(make_directory(STORAGE_MANAGER_ROOT_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions") == 0);
}

static const char *const staged_artifact_names[] = {
    "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
};

static const char *const staged_artifact_contents[] = {
    "scan", "occupancy", "preview", "registration",
};

static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static void write_staged_artifacts(void)
{
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging") == 0);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char path[256];
        const int written = snprintf(
            path,
            sizeof(path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        assert(written > 0 && (size_t)written < sizeof(path));
        FILE *file = fopen(path, "wb");
        assert(file != NULL);
        const size_t length = strlen(staged_artifact_contents[i]);
        assert(fwrite(staged_artifact_contents[i], 1U, length, file) == length);
        assert(fclose(file) == 0);
    }
}

static void assert_file_contents(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    char actual[32] = {0};
    const size_t expected_length = strlen(expected);
    assert(expected_length < sizeof(actual));
    assert(fread(actual, 1U, sizeof(actual), file) == expected_length);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    assert(memcmp(actual, expected, expected_length) == 0);
}

static void remove_layout(void)
{
    char sessions_path[256];
    (void)snprintf(
        sessions_path,
        sizeof(sessions_path),
        "%s/OBJ_0001/sessions",
        STORAGE_MANAGER_OBJECTS_PATH
    );
    DIR *sessions = opendir(sessions_path);
    if (sessions != NULL) {
        struct dirent *entry = NULL;
        while ((entry = readdir(sessions)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char session_path[256];
            char samples_path[256];
            char marker_path[256];
            char metadata_path[256];
            char backup_path[256];
            static const char *const artifact_names[] = {
                "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
            };
            (void)snprintf(session_path, sizeof(session_path), "%s/%s", sessions_path, entry->d_name);
            (void)snprintf(samples_path, sizeof(samples_path), "%s/samples.bin", session_path);
            (void)snprintf(marker_path, sizeof(marker_path), "%s/marker", session_path);
            (void)snprintf(metadata_path, sizeof(metadata_path), "%s/session.json", session_path);
            (void)snprintf(backup_path, sizeof(backup_path), "%s/session.json.bak", session_path);
            (void)remove(samples_path);
            (void)remove(marker_path);
            (void)remove(metadata_path);
            (void)remove(backup_path);
            for (size_t artifact_index = 0U; artifact_index < sizeof(artifact_names) / sizeof(artifact_names[0]); ++artifact_index) {
                char artifact_path[256];
                (void)snprintf(artifact_path, sizeof(artifact_path), "%s/%s", session_path, artifact_names[artifact_index]);
                (void)remove(artifact_path);
            }
            (void)rmdir(session_path);
        }
        (void)closedir(sessions);
    }
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    static const char *const staged_names[] = {
        "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
    };
    for (size_t i = 0U; i < sizeof(staged_names) / sizeof(staged_names[0]); ++i) {
        char staged_path[256];
        (void)snprintf(staged_path, sizeof(staged_path), STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s", staged_names[i]);
        (void)remove(staged_path);
    }
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}

/* Boundary fakes used by the production filesystem implementation. */
esp_err_t storage_manager_operation_begin(void)
{
    ++storage_begin_calls;
    return storage_begin_error;
}

void storage_manager_operation_end(void)
{
    ++storage_end_calls;
}

esp_err_t storage_manager_create_directory(const char *path)
{
    if (fail_create_directory) {
        return ESP_FAIL;
    }
    return make_directory(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_manager_create_directory_exclusive(const char *path)
{
    ++exclusive_directory_calls;
    if (fail_create_directory) {
        return ESP_FAIL;
    }
    errno = 0;
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    ++remove_tree_calls;
    (void)snprintf(removed_tree_path, sizeof(removed_tree_path), "%s", path);
    return fake_remove_tree_error;
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    (void)object_id;
    (void)profile;
    return ESP_OK;
}

static esp_err_t fake_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary,
    bool known_present
)
{
    (void)object_id;
    if (known_present) {
        ++fake_known_present_summary_read_calls;
    }
    else {
        ++fake_summary_read_calls;
    }
    if (fake_summary_error != ESP_OK || summary == NULL) {
        return fake_summary_error;
    }
    if (fake_summary_override_active) {
        *summary = fake_summary_override;
        snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
        return ESP_OK;
    }
    memset(summary, 0, sizeof(*summary));
    snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
    snprintf(summary->source_name, sizeof(summary->source_name), "AD7606");
    summary->complete = true;
    summary->sample_rate_hz = 500U;
    summary->channel_count = SEISMIC_CHANNEL_COUNT;
    summary->samples_written = 5U;
    return ESP_OK;
}

/* Not reached by these tests, but required when this TU includes fs.c. */
esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return fake_read_summary(
        object_id,
        session_id,
        summary,
        false
    );
}

esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return fake_read_summary(
        object_id,
        session_id,
        summary,
        true
    );
}

static size_t recorder_test_fwrite(
    const void *buffer,
    size_t element_size,
    size_t element_count,
    FILE *stream
)
{
    const size_t call_index = recorder_test_fwrite_calls++;
    const size_t requested_bytes = element_size * element_count;
    size_t count = element_count;
    if (injected_short_write_call != SIZE_MAX) {
        if (call_index == injected_short_write_call && injected_short_write_count < count) {
            count = injected_short_write_count;
        }
    }
    else if (injected_short_write_count < count) {
        /* Preserve the original fixture's all-call short-write mode. */
        count = injected_short_write_count;
    }
    const size_t accepted_count = fwrite(buffer, element_size, count, stream);
    if (call_index < RECORDER_TEST_WRITE_TRACE_CAPACITY) {
        recorder_test_fwrite_requested_bytes[call_index] = requested_bytes;
        recorder_test_fwrite_accepted_bytes[call_index] = accepted_count * element_size;
    }
    return accepted_count;
}

static int recorder_test_fflush(FILE *stream)
{
    ++recorder_test_fflush_calls;
    if (injected_fflush_error) return EOF;
    return fflush(stream);
}

static int seismic_recorder_contract_test_fsync(int descriptor)
{
    (void)descriptor;
    ++recorder_test_fsync_calls;
    return injected_fsync_error ? -1 : 0;
}

static int recorder_test_fclose(FILE *stream)
{
    ++recorder_test_fclose_calls;
    const int result = fclose(stream);
    return injected_fclose_error && result == 0 ? EOF : result;
}

#define fwrite recorder_test_fwrite
#define fflush recorder_test_fflush
#ifdef _WIN32
#define _commit seismic_recorder_contract_test_fsync
#else
#define fsync seismic_recorder_contract_test_fsync
#endif
#define fclose recorder_test_fclose
#define rename seismic_recorder_contract_test_rename
#define opendir seismic_recorder_test_opendir
#define readdir seismic_recorder_test_readdir
#define closedir seismic_recorder_test_closedir
#define SEISMIC_RECORDER_SAMPLES_HOST_TEST 1
#include "../seismic_recorder_samples.c"
#undef SEISMIC_RECORDER_SAMPLES_HOST_TEST
#include "../seismic_recorder_fs.c"
#include "../seismic_recorder_reader.c"
#undef fwrite
#undef fflush
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
#undef fclose
#undef rename
#undef opendir
#undef readdir
#undef closedir

bool seismic_recorder_cache_contains_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    uint64_t frame_count
)
{
    (void)object_id;
    (void)session_id;
    (void)first_session_frame;
    (void)frame_count;
    return false;
}

esp_err_t seismic_recorder_cache_copy_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
)
{
    (void)object_id;
    (void)session_id;
    (void)first_session_frame;
    (void)frames;
    (void)frame_count;
    return ESP_ERR_NOT_FOUND;
}


static esp_err_t test_fs_create_session(
    const char *object_id,
    uint32_t sample_rate_hz,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
);
static esp_err_t test_fs_create_ad7606_session(
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file,
    uint16_t *format_version
);
static esp_err_t test_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    size_t frame_count,
    size_t *frames_written
);
static esp_err_t test_fs_write_frames_with_source_indices(
    FILE *samples_file,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    size_t *frames_written
);


static void set_fake_range_summary(
    const char *session_id,
    uint16_t format_version,
    uint64_t samples_written
)
{
    memset(&fake_summary_override, 0, sizeof(fake_summary_override));
    snprintf(
        fake_summary_override.session_id,
        sizeof(fake_summary_override.session_id),
        "%s",
        session_id
    );
    fake_summary_override.complete = true;
    fake_summary_override.readable = true;
    fake_summary_override.sample_rate_hz = 500U;
    fake_summary_override.channel_count = SEISMIC_CHANNEL_COUNT;
    fake_summary_override.format_version = format_version;
    fake_summary_override.samples_written = samples_written;
    fake_summary_override_active = true;
    fake_summary_error = ESP_OK;
}


static void test_session_range_v1_preserves_timestamps_and_bounds(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    assert(test_fs_create_session(
        "OBJ_0001", 500U, SEISMIC_CHANNEL_COUNT, session, &samples
    ) == ESP_OK);
    seismic_frame_t written[10] = {0};
    for (size_t i = 0U; i < 10U; ++i) {
        written[i].timestamp_us = 123456LL + (int64_t)i * 1987LL;
        written[i].channel_uv[0] = (float)i;
    }
    size_t frames_written = 0U;
    assert(test_fs_write_frames(samples, written, 10U, &frames_written) == ESP_OK);
    assert(frames_written == 10U);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);
    set_fake_range_summary(session, SEISMIC_RECORDER_BINARY_VERSION_V1, 10U);

    seismic_frame_t frames[5] = {0};
    uint64_t indices[5] = {0};
    bool gaps[5] = {false};
    size_t count = 0U;
    seismic_recorder_session_summary_t summary;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 3U, 4U, frames, indices, gaps, &count, &summary
    ) == ESP_OK);
    assert(count == 4U && summary.format_version == SEISMIC_RECORDER_BINARY_VERSION_V1);
    seismic_frame_t ex_frames[5] = {0};
    uint64_t ex_indices[5] = {0};
    bool ex_gaps[5] = {false};
    size_t ex_count = 0U;
    seismic_recorder_raw_window_stats_t raw_stats;
    assert(seismic_recorder_read_session_range_ex(
        "OBJ_0001", session, 3U, 4U, ex_frames, ex_indices, ex_gaps,
        &ex_count, NULL, &raw_stats
    ) == ESP_OK);
    assert(ex_count == count && raw_stats.samples_examined == 0U);
    for (size_t i = 0U; i < count; ++i) {
        assert(frames[i].timestamp_us == written[i + 3U].timestamp_us);
        assert(indices[i] == (uint64_t)i + 3U);
        assert(!gaps[i]);
        assert(ex_frames[i].timestamp_us == frames[i].timestamp_us);
        assert(ex_indices[i] == indices[i] && ex_gaps[i] == gaps[i]);
        assert(memcmp(ex_frames[i].channel_uv, frames[i].channel_uv,
                      sizeof(frames[i].channel_uv)) == 0);
    }

    count = 99U;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 11U, 1U, frames, indices, gaps, &count, NULL
    ) == ESP_ERR_INVALID_ARG);
    assert(count == 0U);
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 10U, 1U, frames, indices, gaps, &count, NULL
    ) == ESP_OK);
    assert(count == 0U);

    fake_summary_override_active = false;
    fake_summary_error = ESP_FAIL;
    remove_layout();
}


static void test_session_range_v2_seeks_blocks_and_preserves_gaps(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    const size_t total = 262U;
    seismic_frame_t written[262] = {0};
    uint64_t source_indices[262] = {0};
    for (size_t i = 0U; i < total; ++i) {
        source_indices[i] = i < 258U ? (uint64_t)i : (uint64_t)i + 3U;
        written[i].timestamp_us = (int64_t)source_indices[i] * 2000LL;
        written[i].channel_uv[0] = (float)(int)(i % 100U);
    }
    size_t frames_written = 0U;
    assert(test_fs_write_frames_with_source_indices(
        samples, written, source_indices, total, &frames_written
    ) == ESP_OK);
    assert(frames_written == total);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);
    set_fake_range_summary(session, version, total);

    seismic_frame_t frames[8] = {0};
    uint64_t indices[8] = {0};
    bool gaps[8] = {false};
    size_t count = 0U;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 254U, 8U, frames, indices, gaps, &count, NULL
    ) == ESP_OK);
    assert(count == 8U);
    seismic_frame_t ex_frames[8] = {0};
    uint64_t ex_indices[8] = {0};
    bool ex_gaps[8] = {false};
    size_t ex_count = 0U;
    seismic_recorder_raw_window_stats_t raw_stats;
    assert(seismic_recorder_read_session_range_ex(
        "OBJ_0001", session, 254U, 8U, ex_frames, ex_indices, ex_gaps,
        &ex_count, NULL, &raw_stats
    ) == ESP_OK);
    assert(ex_count == count && raw_stats.samples_examined == count);
    const uint64_t expected_indices[8] = {254U, 255U, 256U, 257U, 261U, 262U, 263U, 264U};
    for (size_t i = 0U; i < count; ++i) {
        assert(indices[i] == expected_indices[i]);
        assert(frames[i].timestamp_us == (int64_t)indices[i] * 2000LL);
        assert(gaps[i] == (i == 4U));
        assert(ex_indices[i] == indices[i] && ex_gaps[i] == gaps[i]);
        assert(ex_frames[i].timestamp_us == frames[i].timestamp_us);
        assert(memcmp(ex_frames[i].channel_uv, frames[i].channel_uv,
                      sizeof(frames[i].channel_uv)) == 0);
    }

    /* An early range must not inspect the later block's header or payload. */
    char path[SEISMIC_RECORDER_PATH_SIZE];
    assert(seismic_recorder_fs_build_session_path(
        "OBJ_0001", session, path, sizeof(path)
    ) == ESP_OK);
    const size_t path_length = strlen(path);
    assert(path_length + sizeof("/samples.bin") <= sizeof(path));
    memcpy(path + path_length, "/samples.bin", sizeof("/samples.bin"));
    FILE *corrupt = fopen(path, "r+b");
    assert(corrupt != NULL);
    const long final_block_header_offset = (long)(
        sizeof(seismic_recorder_binary_header_t) +
        sizeof(seismic_recorder_binary_v2_metadata_t) +
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES +
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    );
    assert(fseek(corrupt, final_block_header_offset + 16L, SEEK_SET) == 0);
    const uint16_t invalid_frame_count = 0U;
    assert(fwrite(&invalid_frame_count, sizeof(invalid_frame_count), 1U, corrupt) == 1U);
    assert(fclose(corrupt) == 0);

    count = 0U;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 0U, 1U, frames, indices, gaps, &count, NULL
    ) == ESP_OK);
    assert(count == 1U && indices[0] == 0U && !gaps[0]);

    fake_summary_override_active = false;
    fake_summary_error = ESP_FAIL;
    remove_layout();
}

static esp_err_t test_fs_create_session(
    const char *object_id,
    uint32_t sample_rate_hz,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "TEST",
        .sample_rate_hz = sample_rate_hz,
    };
    uint16_t version = 0U;
    return seismic_recorder_fs_create_session(
        object_id,
        &acquisition,
        channel_count,
        session_id,
        samples_file,
        &version
    );
}


static esp_err_t test_fs_create_ad7606_session(
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file,
    uint16_t *format_version
)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "AD7606",
        .sample_rate_hz = 500U,
        .bus_clock_hz = 10000000U,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .oversampling_ratio = 4U,
        .input_range_v = 5.0f,
    };
    return seismic_recorder_fs_create_session(
        "OBJ_0001",
        &acquisition,
        SEISMIC_CHANNEL_COUNT,
        session_id,
        samples_file,
        format_version
    );
}


static esp_err_t test_fs_write_frames_with_source_indices(
    FILE *samples_file,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    size_t *frames_written
)
{
    return seismic_recorder_fs_write_frames(
        samples_file,
        frames,
        sample_indices,
        frame_count,
        frames_written
    );
}


static esp_err_t test_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    size_t frame_count,
    size_t *frames_written
)
{
    uint64_t sample_indices[4096];
    if (frame_count > sizeof(sample_indices) / sizeof(sample_indices[0])) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t index = 0U; index < frame_count; ++index) {
        sample_indices[index] = (uint64_t)index + 1U;
    }
    return seismic_recorder_fs_write_frames(
        samples_file,
        frames,
        sample_indices,
        frame_count,
        frames_written
    );
}


#define seismic_recorder_fs_create_session test_fs_create_session
#define seismic_recorder_fs_write_frames test_fs_write_frames

static int seismic_recorder_contract_test_rename(const char *source, const char *destination)
{
    if (injected_rename_source[0] != '\0' && strcmp(source, injected_rename_source) == 0) {
        injected_rename_source[0] = '\0';
        errno = EIO;
        return -1;
    }
    return rename(source, destination);
}

static void test_invalid_arguments(void)
{
    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session(NULL, 50, 4, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_create_session("OBJ_0001/..", 50, 4, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_create_session("OBJ_0001", 0, 4, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 0, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_write_frames(NULL, NULL, 1, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_flush(NULL, false) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_sync(NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_close(NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_delete_session("OBJ_0001", "S000001/..") == ESP_ERR_INVALID_ARG);
}

static void test_session_id_parser_is_canonical(void)
{
    char path[256];
    assert(
        seismic_recorder_fs_build_session_path(
            "OBJ_0001",
            "S000001",
            path,
            sizeof(path)
        ) == ESP_OK
    );
    assert(
        seismic_recorder_fs_build_session_path(
            "OBJ_0001",
            "S999999",
            path,
            sizeof(path)
        ) == ESP_OK
    );

    const char *invalid_ids[] = {
        "S1",
        "S00000a",
        "S000001x",
        "S000000",
        "S1000000",
    };
    for (size_t index = 0U; index < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++index) {
        assert(
            seismic_recorder_fs_build_session_path(
                "OBJ_0001",
                invalid_ids[index],
                path,
                sizeof(path)
            ) == ESP_ERR_INVALID_ARG
        );
    }
}

static void test_session_header_and_frames(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0;
    storage_end_calls = 0;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(file != NULL);
    assert(storage_begin_calls == 1U);
    assert(storage_end_calls == 1U);

    seismic_frame_t frames[2] = {
        {.timestamp_us = 100, .channel_uv = {1.0f, 2.0f, 3.0f, 4.0f}},
        {.timestamp_us = 200, .channel_uv = {-1.0f, -2.0f, -3.0f, -4.0f}},
    };
    size_t frames_written = 0;
    assert(seismic_recorder_fs_write_frames(file, frames, 2, &frames_written) == ESP_OK);
    assert(frames_written == 2U);
    assert(storage_begin_calls == 2U);
    assert(storage_end_calls == 2U);
    assert(seismic_recorder_fs_flush(file, false) == ESP_OK);
    assert(seismic_recorder_fs_sync(file) == ESP_OK);
    assert(storage_begin_calls == 4U);
    assert(storage_end_calls == 4U);
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    assert(storage_begin_calls == 5U);
    assert(storage_end_calls == 5U);

    file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin", "rb");
    assert(file != NULL);
    seismic_recorder_binary_header_t header;
    assert(fread(&header, sizeof(header), 1, file) == 1U);
    assert(memcmp(header.magic, "GEOSEIS1", 8) == 0);
    assert(header.version == 1U);
    assert(header.channel_count == 4U);
    assert(header.sample_rate_hz == 500U);
    assert(sizeof(seismic_frame_t) == 24U);
    assert(sizeof(seismic_recorder_binary_header_t) == 24U);
    assert(header.frame_size_bytes == 24U);
    seismic_frame_t read_frames[2] = {0};
    assert(fread(read_frames, sizeof(read_frames[0]), 2, file) == 2U);
    assert(read_frames[0].timestamp_us == 100);
    assert(read_frames[1].channel_uv[3] == -4.0f);
    assert(fclose(file) == 0);
    remove_layout();
}


static int16_t test_read_i16_le(const uint8_t *bytes)
{
    const uint16_t raw = (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
    int16_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}


static esp_err_t test_file_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    FILE *file = (FILE *)context;
    return
        offset_bytes <= (uint64_t)LONG_MAX &&
        fseek(file, (long)offset_bytes, SEEK_SET) == 0 &&
        fread(buffer, 1U, size_bytes, file) == size_bytes
            ? ESP_OK
            : ESP_ERR_INVALID_RESPONSE;
}


static void recorder_test_reset_io_trace(void)
{
    recorder_test_fwrite_calls = 0U;
    recorder_test_fflush_calls = 0U;
    recorder_test_fsync_calls = 0U;
    recorder_test_fclose_calls = 0U;
    memset(recorder_test_fwrite_requested_bytes, 0, sizeof(recorder_test_fwrite_requested_bytes));
    memset(recorder_test_fwrite_accepted_bytes, 0, sizeof(recorder_test_fwrite_accepted_bytes));
}


static bool recorder_test_write_budget_matches(
    uint16_t format_version,
    const uint64_t *sample_indices,
    size_t frame_count,
    size_t call_count,
    const size_t *requested_bytes,
    const size_t *accepted_bytes
)
{
    if (sample_indices == NULL || requested_bytes == NULL || accepted_bytes == NULL) {
        return false;
    }

    size_t expected_calls = 0U;
    size_t frame_offset = 0U;
    while (frame_offset < frame_count) {
        const size_t remaining = frame_count - frame_offset;
        const size_t chunk = remaining < SEISMIC_RECORDER_WRITE_STAGING_FRAMES
            ? remaining
            : SEISMIC_RECORDER_WRITE_STAGING_FRAMES;
        size_t expected_bytes = 0U;
        if (format_version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
            expected_bytes = chunk * sizeof(seismic_frame_t);
        }
        else if (format_version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
            size_t block_count = 1U;
            for (size_t frame = 1U; frame < chunk; ++frame) {
                const uint64_t previous = sample_indices[frame_offset + frame - 1U];
                if (
                    previous == UINT64_MAX ||
                    sample_indices[frame_offset + frame] != previous + 1U
                ) {
                    ++block_count;
                }
            }
            expected_bytes =
                chunk * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES +
                block_count * SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES;
        }
        else {
            return false;
        }

        if (
            expected_calls >= call_count ||
            expected_calls >= RECORDER_TEST_WRITE_TRACE_CAPACITY ||
            requested_bytes[expected_calls] != expected_bytes ||
            accepted_bytes[expected_calls] != expected_bytes
        ) {
            return false;
        }
        ++expected_calls;
        frame_offset += chunk;
    }

    /* The production FS loop emits one fwrite per bounded staging window. */
    return expected_calls == call_count;
}


static bool recorder_test_production_write_budget_matches(
    uint16_t format_version,
    const uint64_t *sample_indices,
    size_t frame_count
)
{
    return recorder_test_write_budget_matches(
        format_version,
        sample_indices,
        frame_count,
        recorder_test_fwrite_calls,
        recorder_test_fwrite_requested_bytes,
        recorder_test_fwrite_accepted_bytes
    );
}


static void fill_write_contract_frames(
    seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count
)
{
    const float scale_uv_per_lsb = 5000000.0f / 32768.0f;
    for (size_t frame = 0U; frame < frame_count; ++frame) {
        frames[frame].timestamp_us = 1000000LL +
            (int64_t)(sample_indices[frame] - sample_indices[0]) * 2000LL;
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const int32_t raw = (int32_t)((frame * 17U + channel * 4001U) % 60001U) - 30000;
            frames[frame].channel_uv[channel] = (float)raw * scale_uv_per_lsb;
        }
    }
}


static void assert_session_decodes_records(
    const char *session_id,
    uint16_t format_version,
    const seismic_frame_t *expected_frames,
    const uint64_t *expected_sample_indices,
    size_t frame_count
)
{
    char path[256];
    const int path_size = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
        session_id
    );
    assert(path_size > 0 && (size_t)path_size < sizeof(path));
    FILE *samples = fopen(path, "rb");
    assert(samples != NULL);
    assert(fseek(samples, 0L, SEEK_END) == 0);
    const long file_size = ftell(samples);
    assert(file_size >= 0L);

    seismic_recorder_session_summary_t summary = {0};
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.format_version = format_version;
    summary.samples_written = frame_count;
    summary.complete = false;
    summary.recovered = false;
    seismic_recorder_samples_reader_t reader;
    assert(seismic_recorder_samples_reader_open(
        test_file_read_at,
        samples,
        (uint64_t)file_size,
        &summary,
        &reader
    ) == ESP_OK);

    const float scale_uv_per_lsb = 5000000.0f / 32768.0f;
    size_t logical_position = 0U;
    while (logical_position < frame_count) {
        seismic_frame_t decoded[SEISMIC_RECORDER_WRITE_STAGING_FRAMES] = {0};
        size_t decoded_count = 0U;
        uint64_t first_sample_index = UINT64_MAX;
        bool gap_before = false;
        assert(seismic_recorder_samples_reader_next(
            test_file_read_at,
            samples,
            &reader,
            decoded,
            SEISMIC_RECORDER_WRITE_STAGING_FRAMES,
            &decoded_count,
            &first_sample_index,
            &gap_before
        ) == ESP_OK);
        assert(decoded_count > 0U && decoded_count <= frame_count - logical_position);
        if (format_version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
            assert(first_sample_index == expected_sample_indices[logical_position]);
            const bool expected_gap = logical_position > 0U &&
                expected_sample_indices[logical_position] >
                    expected_sample_indices[logical_position - 1U] + 1U;
            assert(gap_before == expected_gap);
        }
        for (size_t frame = 0U; frame < decoded_count; ++frame) {
            const size_t expected_position = logical_position + frame;
            assert(decoded[frame].timestamp_us == expected_frames[expected_position].timestamp_us);
            for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                const float tolerance = format_version == SEISMIC_RECORDER_BINARY_VERSION_V2
                    ? scale_uv_per_lsb * 0.5f + 0.01f
                    : 0.0f;
                assert(fabsf(
                    decoded[frame].channel_uv[channel] -
                        expected_frames[expected_position].channel_uv[channel]
                ) <= tolerance);
            }
        }
        logical_position += decoded_count;
    }
    assert(seismic_recorder_samples_reader_finish(
        test_file_read_at,
        samples,
        &reader
    ) == ESP_OK);
    assert(fclose(samples) == 0);
}


#if defined(SEISMIC_RECORDER_GAP_PIPELINE_TEST)
static void publish_synthetic_samples(
    uint64_t first,
    uint64_t end_exclusive,
    bool include_three_sample_gap
)
{
    const float channels[SEISMIC_CHANNEL_COUNT] = {1.0f, 2.0f, 3.0f, 4.0f};
    for (uint64_t index = first; index < end_exclusive; ++index) {
        if (include_three_sample_gap && index >= 100U && index <= 102U) {
            continue;
        }
        assert(seismic_buffer_push_indexed(
            (int64_t)index * 2000LL,
            index,
            channels
        ) == ESP_OK);
    }
}


static size_t copy_ring_into_cache(
    uint64_t *cursor,
    seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    uint64_t *sample_indices,
    size_t max_frames,
    uint64_t *dropped_out
)
{
    size_t frame_count = 0U;
    uint64_t dropped = 0U;
    seismic_buffer_info_t info = {0};
    assert(seismic_buffer_copy_since_with_source_indices(
        cursor,
        frames,
        sample_indices,
        max_frames,
        &frame_count,
        &dropped,
        &info
    ) == ESP_OK);
    if (frame_count > 0U) {
        const uint64_t first_ring_frame =
            *cursor - (uint64_t)frame_count + 1U;
        assert(seismic_recorder_cache_append_with_source_indices(
            cache,
            first_ring_frame,
            frames,
            sample_indices,
            frame_count
        ) == ESP_OK);
    }
    if (dropped_out != NULL) {
        *dropped_out = dropped;
    }
    return frame_count;
}


static size_t persist_cache_batch(
    FILE *samples,
    seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    uint64_t *sample_indices,
    size_t max_frames
)
{
    size_t total_written = 0U;
    for (;;) {
        size_t frame_count = 0U;
        uint64_t first_session_frame = 0U;
        assert(seismic_recorder_cache_copy_dirty_with_source_indices(
            cache,
            frames,
            sample_indices,
            max_frames,
            &frame_count,
            &first_session_frame
        ) == ESP_OK);
        if (frame_count == 0U) {
            break;
        }

        size_t frames_written = 0U;
        assert(test_fs_write_frames_with_source_indices(
            samples,
            frames,
            sample_indices,
            frame_count,
            &frames_written
        ) == ESP_OK);
        assert(frames_written == frame_count);
        uint64_t last_ring_frame = 0U;
        assert(seismic_recorder_cache_mark_persisted(
            cache,
            first_session_frame,
            frames_written,
            &last_ring_frame
        ) == ESP_OK);
        total_written += frames_written;
    }
    return total_written;
}


static void assert_recorded_timestamps_and_gaps(
    FILE *samples,
    const char *session_id,
    uint64_t logical_frames,
    uint64_t expected_missing,
    uint64_t overflow_cutoff
)
{
    assert(seismic_recorder_fs_close(samples) == ESP_OK);
    char path[256];
    const int path_size = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
        session_id
    );
    assert(path_size > 0 && (size_t)path_size < sizeof(path));
    FILE *reader_file = fopen(path, "rb");
    assert(reader_file != NULL);
    assert(fseek(reader_file, 0L, SEEK_END) == 0);
    const long file_size = ftell(reader_file);
    assert(file_size > 0L);

    seismic_recorder_session_summary_t summary = {0};
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.format_version = SEISMIC_RECORDER_BINARY_VERSION_V2;
    summary.samples_written = logical_frames;
    summary.complete = true;
    summary.recovered = true;
    seismic_recorder_samples_reader_t reader;
    const esp_err_t open_err = seismic_recorder_samples_reader_open(
        test_file_read_at,
        reader_file,
        (uint64_t)file_size,
        &summary,
        &reader
    );
    assert(open_err == ESP_OK);

    uint64_t logical_position = 0U;
    unsigned gap_blocks = 0U;
    while (logical_position < logical_frames) {
        seismic_frame_t decoded[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
        size_t decoded_count = 0U;
        uint64_t first_index = 0U;
        bool gap_before = false;
        assert(seismic_recorder_samples_reader_next(
            test_file_read_at,
            reader_file,
            &reader,
            decoded,
            SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
            &decoded_count,
            &first_index,
            &gap_before
        ) == ESP_OK);
        assert(decoded_count > 0U);
        if (gap_before) {
            ++gap_blocks;
        }
        for (size_t index = 0U; index < decoded_count; ++index) {
            const uint64_t source_index = first_index + (uint64_t)index;
            assert(decoded[index].timestamp_us == (int64_t)source_index * 2000LL);
            if (overflow_cutoff != UINT64_MAX && logical_position >= overflow_cutoff) {
                assert(source_index == logical_position + expected_missing);
            }
            else if (overflow_cutoff != UINT64_MAX) {
                assert(source_index == logical_position);
            }
            if (expected_missing == 3U && logical_position >= 100U) {
                assert(source_index == logical_position + 3U);
            }
            ++logical_position;
        }
    }
    assert(reader.has_gaps == (expected_missing > 0U));
    assert(reader.missing_sample_count == expected_missing);
    assert(gap_blocks == 1U);
    assert(seismic_recorder_samples_reader_finish(
        test_file_read_at,
        reader_file,
        &reader
    ) == ESP_OK);
    assert(fclose(reader_file) == 0);
}


static void test_acquisition_gap_survives_ring_cache_v2_reader(void)
{
    remove_layout();
    make_layout();
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    assert(version == SEISMIC_RECORDER_BINARY_VERSION_V2);

    publish_synthetic_samples(0U, 200U, true);
    seismic_recorder_cache_slot_t slots[256];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, slots, 256U) == ESP_OK);
    seismic_frame_t frames[256] = {0};
    uint64_t sample_indices[256] = {0};
    uint64_t cursor = 0U;
    uint64_t dropped = UINT64_MAX;
    const size_t count = copy_ring_into_cache(
        &cursor, &cache, frames, sample_indices, 256U, &dropped
    );
    assert(count == 197U && dropped == 0U);
    assert(persist_cache_batch(samples, &cache, frames, sample_indices, 256U) == 197U);
    assert_recorded_timestamps_and_gaps(samples, session, 197U, 3U, UINT64_MAX);
    remove_layout();
}


static void test_ring_overflow_survives_as_recorded_gap(void)
{
    remove_layout();
    make_layout();
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);

    seismic_recorder_cache_slot_t slots[2200];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, slots, 2200U) == ESP_OK);
    seismic_frame_t frames[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
    uint64_t sample_indices[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
    uint64_t cursor = 0U;
    publish_synthetic_samples(0U, 100U, false);
    assert(copy_ring_into_cache(
        &cursor, &cache, frames, sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES, NULL
    ) == 100U);
    assert(persist_cache_batch(
        samples, &cache, frames, sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES
    ) == 100U);

    const uint64_t capacity = seismic_buffer_capacity();
    assert(capacity == 2048U);
    publish_synthetic_samples(100U, 2200U, false);
    uint64_t dropped = 0U;
    const size_t retained = copy_ring_into_cache(
        &cursor,
        &cache,
        frames,
        sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
        &dropped
    );
    assert(dropped == 52U && retained == SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES);
    while (cursor < 2200U) {
        const size_t count = copy_ring_into_cache(
            &cursor,
            &cache,
            frames,
            sample_indices,
            SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
            NULL
        );
        assert(count > 0U);
    }
    assert(persist_cache_batch(
        samples, &cache, frames, sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES
    ) == 2048U);
    assert_recorded_timestamps_and_gaps(samples, session, 2148U, 52U, 100U);
    remove_layout();
}
#endif


#if defined(MAX371_RECORDER_RAW_ORACLE_HOST_TEST)
static int16_t max371_i16_from_bits(uint16_t bits)
{
    int16_t value = 0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}


static seismic_recorder_sample_format_t max371_ad7606_format(
    uint32_t full_scale_mv,
    uint8_t oversampling_ratio
)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "AD7606",
        .sample_rate_hz = 500U,
        .oversampling_ratio = oversampling_ratio,
        .input_range_v = (float)full_scale_mv / 1000.0f,
    };
    seismic_recorder_sample_format_t format = {0};
    assert(seismic_recorder_samples_format_from_acquisition(&acquisition, &format) == ESP_OK);
    return format;
}


static float max371_ad7606_raw_uv(int16_t raw, uint8_t oversampling_ratio, uint32_t full_scale_mv)
{
    float microvolts = 0.0f;
    assert(ad7606_raw_to_uv_for_config(raw, oversampling_ratio, full_scale_mv, &microvolts));
    return microvolts;
}


static void test_exhaustive_ad7606_codes_round_trip_v2(void)
{
    static const uint32_t full_scale_mv[] = {5000U, 10000U};
    static const uint8_t oversampling_ratios[] = {1U, 2U, 4U, 8U};
    static const int16_t boundary_codes[] = {
        INT16_MIN, INT16_MIN + 1, -1, 0, 1, INT16_MAX - 1, INT16_MAX,
    };
    enum { batch_frames = SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES };
    seismic_frame_t frames[batch_frames] = {0};
    uint64_t sample_indices[batch_frames] = {0};
    int16_t expected_raw[batch_frames][SEISMIC_CHANNEL_COUNT] = {{0}};
    uint8_t encoded[
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        batch_frames * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    ] = {0};

    for (size_t range_index = 0U; range_index < sizeof(full_scale_mv) / sizeof(full_scale_mv[0]); ++range_index) {
        for (
            size_t os_index = 0U;
            os_index < sizeof(oversampling_ratios) / sizeof(oversampling_ratios[0]);
            ++os_index
        ) {
            const uint32_t range_mv = full_scale_mv[range_index];
            const uint8_t os_ratio = oversampling_ratios[os_index];
            const seismic_recorder_sample_format_t format = max371_ad7606_format(range_mv, os_ratio);

            /* These named two's-complement edge codes are also covered by the full sweep. */
            seismic_frame_t edges[sizeof(boundary_codes) / sizeof(boundary_codes[0])] = {0};
            uint64_t edge_indices[sizeof(boundary_codes) / sizeof(boundary_codes[0])] = {0};
            int16_t edge_expected[
                sizeof(boundary_codes) / sizeof(boundary_codes[0])
            ][SEISMIC_CHANNEL_COUNT] = {{0}};
            uint8_t edge_bytes[
                SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
                sizeof(boundary_codes) / sizeof(boundary_codes[0]) *
                    SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
            ] = {0};
            for (size_t frame = 0U; frame < sizeof(boundary_codes) / sizeof(boundary_codes[0]); ++frame) {
                edges[frame].timestamp_us = (int64_t)frame * 2000LL;
                edge_indices[frame] = frame;
                for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                    const int16_t raw = boundary_codes[(frame + channel * 2U) %
                        (sizeof(boundary_codes) / sizeof(boundary_codes[0]))];
                    edge_expected[frame][channel] = raw;
                    edges[frame].channel_uv[channel] =
                        max371_ad7606_raw_uv(raw, os_ratio, range_mv);
                }
            }
            s_test_quantizer_call_count = 0U;
            size_t edge_bytes_written = 0U;
            assert(seismic_recorder_samples_encode_v2_block(
                &format,
                edges,
                edge_indices,
                sizeof(boundary_codes) / sizeof(boundary_codes[0]),
                false,
                edge_bytes,
                sizeof(edge_bytes),
                &edge_bytes_written
            ) == ESP_OK);
            assert(edge_bytes_written == sizeof(edge_bytes));
            for (size_t frame = 0U; frame < sizeof(boundary_codes) / sizeof(boundary_codes[0]); ++frame) {
                for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                    const size_t offset = SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
                        frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * sizeof(int16_t);
                    assert(test_read_i16_le(&edge_bytes[offset]) == edge_expected[frame][channel]);
                }
            }
            assert(s_test_quantizer_call_count ==
                (sizeof(boundary_codes) / sizeof(boundary_codes[0])) * SEISMIC_CHANNEL_COUNT);

            s_test_quantizer_call_count = 0U;
            for (uint32_t first_code = 0U; first_code < 65536U; first_code += batch_frames) {
                for (size_t frame = 0U; frame < batch_frames; ++frame) {
                    const uint32_t source_code = first_code + (uint32_t)frame;
                    const int16_t base_raw = (int16_t)((int32_t)INT16_MIN + (int32_t)source_code);
                    frames[frame].timestamp_us = (int64_t)source_code * 2000LL;
                    sample_indices[frame] = source_code;
                    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                        const uint16_t bits = (uint16_t)(
                            (uint16_t)base_raw + (uint16_t)(channel * 0x4000U)
                        );
                        const int16_t raw = max371_i16_from_bits(bits);
                        expected_raw[frame][channel] = raw;
                        frames[frame].channel_uv[channel] =
                            max371_ad7606_raw_uv(raw, os_ratio, range_mv);
                    }
                }

                size_t bytes_written = 0U;
                assert(seismic_recorder_samples_encode_v2_block(
                    &format,
                    frames,
                    sample_indices,
                    batch_frames,
                    false,
                    encoded,
                    sizeof(encoded),
                    &bytes_written
                ) == ESP_OK);
                assert(bytes_written == sizeof(encoded));
                for (size_t frame = 0U; frame < batch_frames; ++frame) {
                    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                        const size_t offset = SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
                            frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * sizeof(int16_t);
                        assert(test_read_i16_le(&encoded[offset]) == expected_raw[frame][channel]);
                    }
                }
                if (range_index == 0U && os_index == 0U && first_code == 0U) {
                    assert(s_test_quantizer_call_count == batch_frames * SEISMIC_CHANNEL_COUNT);
                    printf(
                        "MAX371_V2_ENCODER_BASELINE frames=%u channels=%u calls=%u calls_per_frame=%u\n",
                        (unsigned)batch_frames,
                        (unsigned)SEISMIC_CHANNEL_COUNT,
                        (unsigned)s_test_quantizer_call_count,
                        (unsigned)(s_test_quantizer_call_count / batch_frames)
                    );
                }
            }
            assert(s_test_quantizer_call_count == 65536U * SEISMIC_CHANNEL_COUNT);
        }
    }
}


static void test_nonzero_offset_is_not_raw_reversible(void)
{
    const seismic_recorder_sample_format_t base_format = max371_ad7606_format(5000U, 4U);
    seismic_recorder_sample_format_t calibrated_format = base_format;
    static const int16_t input_raw[SEISMIC_CHANNEL_COUNT] = {
        INT16_MIN, -1, 0, INT16_MAX,
    };
    seismic_frame_t frame = {.timestamp_us = 1000000LL};
    uint64_t sample_index = 11U;
    uint8_t encoded[
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES + SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    ] = {0};
    size_t bytes_written = 0U;

    /* A +1 LSB offset is a valid calibration, but changes the stored raw code. */
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        calibrated_format.offset_uv[channel] = calibrated_format.scale_uv_per_lsb[channel];
        frame.channel_uv[channel] = max371_ad7606_raw_uv(input_raw[channel], 4U, 5000U);
    }
    s_test_quantizer_call_count = 0U;
    assert(seismic_recorder_samples_encode_v2_block(
        &calibrated_format,
        &frame,
        &sample_index,
        1U,
        false,
        encoded,
        sizeof(encoded),
        &bytes_written
    ) == ESP_OK);
    assert(bytes_written == sizeof(encoded));
    assert(s_test_quantizer_call_count == SEISMIC_CHANNEL_COUNT);
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        const size_t offset = SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES + channel * sizeof(int16_t);
        const int16_t expected = input_raw[channel] == INT16_MIN
            ? INT16_MIN
            : (int16_t)(input_raw[channel] - 1);
        const int16_t encoded_raw = test_read_i16_le(&encoded[offset]);
        assert(encoded_raw == expected);
        if (input_raw[channel] == INT16_MIN) {
            assert(encoded_raw == input_raw[channel]); /* Saturation keeps the raw endpoint. */
        }
        else {
            assert(encoded_raw != input_raw[channel]);
        }
    }
}


static void test_max371_v2_file_staging_boundaries_and_stop_tail(void)
{
    enum {
        total_frames = 1U + (SEISMIC_RECORDER_WRITE_STAGING_FRAMES - 1U) +
            SEISMIC_RECORDER_WRITE_STAGING_FRAMES +
            (SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 1U) +
            (2U * SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 5U),
    };
    static const size_t write_sizes[] = {
        1U,
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES - 1U,
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES,
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 1U,
        2U * SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 5U,
    };
    /* Gap at 101 is inside a block; 255/256/257 straddle the 256-frame boundary. */
    static const size_t gap_before_frames[] = {101U, 255U, 256U, 257U};
    const uint32_t full_scale_mv = 5000U;
    const uint8_t oversampling_ratio = 4U;
    const float scale_uv_per_lsb = 5000000.0f / 32768.0f;
    const uint64_t first_source_index = 10000U;
    const int64_t first_timestamp_us = 1000000LL;
    seismic_frame_t frames[total_frames] = {0};
    uint64_t sample_indices[total_frames] = {0};
    size_t missing_samples = 0U;
    uint64_t source_index_offset = 0U;

    for (size_t frame = 0U; frame < total_frames; ++frame) {
        for (size_t gap = 0U; gap < sizeof(gap_before_frames) / sizeof(gap_before_frames[0]); ++gap) {
            if (frame == gap_before_frames[gap]) {
                ++source_index_offset;
                ++missing_samples;
            }
        }
        sample_indices[frame] = first_source_index + (uint64_t)frame + source_index_offset;
        frames[frame].timestamp_us = first_timestamp_us +
            (int64_t)(sample_indices[frame] - first_source_index) * 2000LL;
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const uint16_t bits = (uint16_t)(
                (uint32_t)(frame * 73U + 0x1234U) + (uint32_t)(channel * 0x4000U)
            );
            frames[frame].channel_uv[channel] = max371_ad7606_raw_uv(
                max371_i16_from_bits(bits),
                oversampling_ratio,
                full_scale_mv
            );
        }
    }

    remove_layout();
    make_layout();
    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    assert(version == SEISMIC_RECORDER_BINARY_VERSION_V2);

    size_t position = 0U;
    size_t staging_write_calls = 0U;
    size_t staging_write_bytes = 0U;
    size_t staging_write_sizes[8] = {0};
    for (size_t write = 0U; write < sizeof(write_sizes) / sizeof(write_sizes[0]); ++write) {
        const size_t count = write_sizes[write];
        size_t written = 0U;
        recorder_test_reset_io_trace();
        s_test_quantizer_call_count = 0U;
        assert(test_fs_write_frames_with_source_indices(
            samples,
            &frames[position],
            &sample_indices[position],
            count,
            &written
        ) == ESP_OK);
        assert(written == count);
        assert(recorder_test_production_write_budget_matches(
            SEISMIC_RECORDER_BINARY_VERSION_V2,
            &sample_indices[position],
            count
        ));
        assert(staging_write_calls + recorder_test_fwrite_calls <= sizeof(staging_write_sizes) / sizeof(staging_write_sizes[0]));
        for (size_t call = 0U; call < recorder_test_fwrite_calls; ++call) {
            staging_write_sizes[staging_write_calls + call] =
                recorder_test_fwrite_requested_bytes[call];
            staging_write_bytes += recorder_test_fwrite_requested_bytes[call];
        }
        staging_write_calls += recorder_test_fwrite_calls;
        /* The live FS fixture also quantizes each frame once for its active LOD sidecar. */
        assert(s_test_quantizer_call_count == count * SEISMIC_CHANNEL_COUNT * 2U);
        if (count == SEISMIC_RECORDER_WRITE_STAGING_FRAMES) {
            /* Four v2 codes plus four LOD inputs are quantized per frame in this fixture. */
            printf(
                "MAX371_FS_QUANTIZER_BASELINE frames=%u sample_channels=%u calls=%u calls_per_frame=%u lod_active=1\n",
                (unsigned)count,
                (unsigned)SEISMIC_CHANNEL_COUNT,
                (unsigned)s_test_quantizer_call_count,
                (unsigned)(s_test_quantizer_call_count / count)
            );
            assert(s_test_quantizer_call_count == 2048U);
        }
        position += count;
    }
    assert(position == total_frames);
    /* 1/255/256/257/517 frames make 1/1/1/2/3 writes: 8 calls, 10,508 bytes (1,286*8 + 11*20 gap/staging headers). */
    assert(staging_write_calls == 8U && staging_write_bytes == 10508U);
    /* Exact v2 fwrite bytes: [28,2100,2088,2068,28,2068,2068,60]; file header+metadata adds 68 bytes. */
    static const size_t expected_staging_write_sizes[] = {
        28U, 2100U, 2088U, 2068U, 28U, 2068U, 2068U, 60U,
    };
    for (size_t call = 0U; call < staging_write_calls; ++call) {
        assert(staging_write_sizes[call] == expected_staging_write_sizes[call]);
    }

    /* The final 2N+5 STOP batch persists two full windows and one five-frame tail. */
    assert(seismic_recorder_fs_flush(samples, false) == ESP_OK);
    assert(seismic_recorder_fs_sync(samples) == ESP_OK);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char path[256];
    const int path_size = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
        session
    );
    assert(path_size > 0 && (size_t)path_size < sizeof(path));
    samples = fopen(path, "rb");
    assert(samples != NULL);
    assert(fseek(samples, 0L, SEEK_END) == 0);
    const long file_size = ftell(samples);
    assert(file_size > 0L);

    seismic_recorder_session_summary_t summary = {0};
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.format_version = SEISMIC_RECORDER_BINARY_VERSION_V2;
    summary.samples_written = total_frames;
    summary.complete = true;
    summary.recovered = true;
    seismic_recorder_samples_reader_t reader;
    assert(seismic_recorder_samples_reader_open(
        test_file_read_at,
        samples,
        (uint64_t)file_size,
        &summary,
        &reader
    ) == ESP_OK);

    size_t logical_position = 0U;
    size_t decoded_blocks = 0U;
    size_t final_block_frames = 0U;
    while (logical_position < total_frames) {
        seismic_frame_t decoded[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
        size_t decoded_count = 0U;
        uint64_t first_index = 0U;
        bool gap_before = false;
        assert(seismic_recorder_samples_reader_next(
            test_file_read_at,
            samples,
            &reader,
            decoded,
            SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
            &decoded_count,
            &first_index,
            &gap_before
        ) == ESP_OK);
        assert(decoded_count > 0U && decoded_count <= total_frames - logical_position);
        assert(first_index == sample_indices[logical_position]);
        const bool expected_gap_before = logical_position > 0U &&
            sample_indices[logical_position] > sample_indices[logical_position - 1U] + 1U;
        assert(gap_before == expected_gap_before);
        for (size_t frame = 0U; frame < decoded_count; ++frame) {
            const size_t expected_position = logical_position + frame;
            assert(first_index + frame == sample_indices[expected_position]);
            assert(decoded[frame].timestamp_us == frames[expected_position].timestamp_us);
            for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                assert(fabsf(
                    decoded[frame].channel_uv[channel] - frames[expected_position].channel_uv[channel]
                ) <= scale_uv_per_lsb * 0.5f);
            }
        }
        logical_position += decoded_count;
        final_block_frames = decoded_count;
        ++decoded_blocks;
    }
    assert(decoded_blocks > 8U);
    assert(final_block_frames == 5U);
    assert(reader.has_gaps && reader.missing_sample_count == missing_samples);
    assert(seismic_recorder_samples_reader_finish(test_file_read_at, samples, &reader) == ESP_OK);
    assert(fclose(samples) == 0);
    remove_layout();
}
#endif


static esp_err_t recorder_test_create_format_session(
    uint16_t requested_version,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
)
{
    if (requested_version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        return test_fs_create_session(
            "OBJ_0001",
            500U,
            SEISMIC_CHANNEL_COUNT,
            session_id,
            samples_file
        );
    }

    uint16_t actual_version = 0U;
    const esp_err_t err = test_fs_create_ad7606_session(
        session_id,
        samples_file,
        &actual_version
    );
    assert(err != ESP_OK || actual_version == requested_version);
    return err;
}


static void test_recorder_batch_and_staging_write_budget(void)
{
    static const size_t write_sizes[] = {
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES - 1U,
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES,
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 1U,
        SEISMIC_RECORDER_BATCH_FRAMES - 1U,
        SEISMIC_RECORDER_BATCH_FRAMES,
        SEISMIC_RECORDER_BATCH_FRAMES + 1U,
        5U,
    };
    static const uint16_t format_versions[] = {
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    seismic_frame_t frames[SEISMIC_RECORDER_BATCH_FRAMES + 1U] = {0};
    uint64_t sample_indices[SEISMIC_RECORDER_BATCH_FRAMES + 1U] = {0};

    /* samples.bin bytes: v1=24+24*frames; v2=68+8*frames+20*blocks (each block is at most 256 contiguous frames). */
    /* V1 is 24 B/frame (timestamp+4 floats); v2 is 8 B/frame plus 20 B/block (source index/timestamp/count/flags) and 68 B/session. */
    /* Sustained v1/v2 bytes/s at 500/5k/10k/20k/40k frames/s are 12k/120k/240k/480k/960k and 4,039/40,391/80,781/161,563/323,125. */
    /* Optional LOD64 adds 32+64*floor(frames/64) bytes, about 1 B/frame; add 0.5/5/10/20/40 kB/s to those totals when active. */
    /* The 500 ms target is 250 frames at 500 Hz and clamps to 512 at 5/10/20/40 kHz; recorder writes at most 256 frames per turn. */
    /* Normal 500 Hz writes are 1x250 frames (6,000/2,020 B v1/v2); a 512-frame batch is 2x256 (6,144/2,068 B each if gap-free). */

    remove_layout();
    make_layout();
    for (size_t format = 0U; format < sizeof(format_versions) / sizeof(format_versions[0]); ++format) {
        for (size_t size_index = 0U; size_index < sizeof(write_sizes) / sizeof(write_sizes[0]); ++size_index) {
            const size_t frame_count = write_sizes[size_index];
            for (size_t frame = 0U; frame < frame_count; ++frame) {
                sample_indices[frame] = 10000U + (uint64_t)frame;
            }
            fill_write_contract_frames(frames, sample_indices, frame_count);

            char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
            FILE *samples = NULL;
            const uint16_t version = format_versions[format];
            assert(recorder_test_create_format_session(version, session, &samples) == ESP_OK);
            recorder_test_reset_io_trace();

            size_t frames_written = 0U;
            assert(test_fs_write_frames_with_source_indices(
                samples,
                frames,
                sample_indices,
                frame_count,
                &frames_written
            ) == ESP_OK);
            assert(frames_written == frame_count);
            assert(recorder_test_production_write_budget_matches(
                version,
                sample_indices,
                frame_count
            ));
            /* 255/256/257 => 1/1/2 calls; 511/512/513 => 2/2/3. V1 caps at 6,144 B/write; v2 contiguous blocks cap at 2,068 B/write. */
            /* Exact 511/512/513-frame traces: v1 [6144,6120]/[6144,6144]/[6144,6144,24]; v2 [2068,2060]/[2068,2068]/[2068,2068,28] B. */
            assert(recorder_test_fwrite_calls ==
                (frame_count + SEISMIC_RECORDER_WRITE_STAGING_FRAMES - 1U) /
                    SEISMIC_RECORDER_WRITE_STAGING_FRAMES);
            assert(recorder_test_fflush_calls == 0U && recorder_test_fsync_calls == 0U);
            assert(seismic_recorder_fs_close(samples) == ESP_OK);
            assert_session_decodes_records(
                session,
                version,
                frames,
                sample_indices,
                frame_count
            );
        }
    }
    remove_layout();
}


static void test_write_budget_rejects_per_frame_debatching(void)
{
    enum { logical_frames = SEISMIC_RECORDER_BATCH_FRAMES };
    uint64_t sample_indices[logical_frames] = {0};
    size_t split_requested_bytes[logical_frames] = {0};
    size_t split_accepted_bytes[logical_frames] = {0};
    for (size_t frame = 0U; frame < logical_frames; ++frame) {
        sample_indices[frame] = 20000U + (uint64_t)frame;
        /* Negative control: a pathological writer issues one fwrite per frame. */
        split_requested_bytes[frame] = sizeof(seismic_frame_t);
        split_accepted_bytes[frame] = sizeof(seismic_frame_t);
    }
    assert(!recorder_test_write_budget_matches(
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        sample_indices,
        logical_frames,
        logical_frames,
        split_requested_bytes,
        split_accepted_bytes
    ));
    for (size_t frame = 0U; frame < logical_frames; ++frame) {
        split_requested_bytes[frame] =
            SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES;
        split_accepted_bytes[frame] = split_requested_bytes[frame];
    }
    assert(!recorder_test_write_budget_matches(
        SEISMIC_RECORDER_BINARY_VERSION_V2,
        sample_indices,
        logical_frames,
        logical_frames,
        split_requested_bytes,
        split_accepted_bytes
    ));
    printf(
        "MAX365_WRITE_BUDGET negative_control=detected logical_frames=512 normal_calls=2 split_calls=512\n"
    );
}


static void test_short_write_positions_preserve_decodable_records(void)
{
    enum { frame_count = 2U * SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 5U };
    seismic_frame_t frames[frame_count] = {0};
    uint64_t sample_indices[frame_count] = {0};
    for (size_t frame = 0U; frame < frame_count; ++frame) {
        sample_indices[frame] = 30000U + (uint64_t)frame;
    }
    fill_write_contract_frames(frames, sample_indices, frame_count);

    static const uint16_t format_versions[] = {
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    for (size_t format = 0U; format < sizeof(format_versions) / sizeof(format_versions[0]); ++format) {
        const uint16_t version = format_versions[format];
        for (size_t failed_call = 0U; failed_call < 3U; ++failed_call) {
            const size_t failed_call_frames = failed_call < 2U
                ? SEISMIC_RECORDER_WRITE_STAGING_FRAMES
                : 5U;
            const size_t accepted_frames = failed_call < 2U ? 128U : 4U;
            const size_t accepted_elements = version == SEISMIC_RECORDER_BINARY_VERSION_V1
                ? accepted_frames
                : SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
                    accepted_frames * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;

            remove_layout();
            make_layout();
            char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
            FILE *samples = NULL;
            assert(recorder_test_create_format_session(version, session, &samples) == ESP_OK);
            recorder_test_reset_io_trace();
            injected_short_write_count = accepted_elements;
            injected_short_write_call = failed_call;

            size_t frames_written = 0U;
            assert(test_fs_write_frames_with_source_indices(
                samples,
                frames,
                sample_indices,
                frame_count,
                &frames_written
            ) == ESP_FAIL);
            const size_t writer_reported_frames = version == SEISMIC_RECORDER_BINARY_VERSION_V1
                ? failed_call * SEISMIC_RECORDER_WRITE_STAGING_FRAMES + accepted_frames
                : failed_call * SEISMIC_RECORDER_WRITE_STAGING_FRAMES;
            assert(frames_written == writer_reported_frames);
            assert(recorder_test_fwrite_calls == failed_call + 1U);
            for (size_t call = 0U; call <= failed_call; ++call) {
                const size_t call_frames = call < 2U
                    ? SEISMIC_RECORDER_WRITE_STAGING_FRAMES
                    : failed_call_frames;
                const size_t requested_bytes = version == SEISMIC_RECORDER_BINARY_VERSION_V1
                    ? call_frames * sizeof(seismic_frame_t)
                    : SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
                        call_frames * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;
                const size_t expected_accepted_bytes = call == failed_call
                    ? (version == SEISMIC_RECORDER_BINARY_VERSION_V1
                        ? accepted_frames * sizeof(seismic_frame_t)
                        : accepted_elements)
                    : requested_bytes;
                assert(recorder_test_fwrite_requested_bytes[call] == requested_bytes);
                assert(recorder_test_fwrite_accepted_bytes[call] == expected_accepted_bytes);
            }

            injected_short_write_count = SIZE_MAX;
            injected_short_write_call = SIZE_MAX;
            assert(seismic_recorder_fs_close(samples) == ESP_OK);

            size_t decoded_records = writer_reported_frames;
            if (version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
                char path[256];
                const int path_size = snprintf(
                    path,
                    sizeof(path),
                    STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
                    session
                );
                assert(path_size > 0 && (size_t)path_size < sizeof(path));
                FILE *recovery_file = fopen(path, "r+b");
                assert(recovery_file != NULL);
                assert(fseek(recovery_file, 0L, SEEK_END) == 0);
                const long file_size = ftell(recovery_file);
                assert(file_size >= 0L);
                uint64_t recovered_frames = 0U;
                uint64_t target_size_bytes = 0U;
                uint16_t recovered_version = 0U;
                assert(seismic_recorder_samples_recover_file(
                    recovery_file,
                    (uint64_t)file_size,
                    500U,
                    SEISMIC_CHANNEL_COUNT,
                    &recovered_frames,
                    &target_size_bytes,
                    &recovered_version
                ) == ESP_OK);
                assert(recovered_version == version);
                assert(recovered_frames == failed_call * SEISMIC_RECORDER_WRITE_STAGING_FRAMES + accepted_frames);
                assert(target_size_bytes <= (uint64_t)file_size);
                decoded_records = (size_t)recovered_frames;
                assert(fclose(recovery_file) == 0);
            }
            /* A failed V2 fwrite reports only complete windows; recovery decodes aligned frames from its partial tail. */
            assert_session_decodes_records(
                session,
                version,
                frames,
                sample_indices,
                decoded_records
            );
            remove_layout();
        }
    }
}


static void test_writer_flush_sync_and_close_call_cadence(void)
{
    static const uint16_t format_versions[] = {
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    const uint64_t sample_indices[5] = {40000U, 40001U, 40002U, 40003U, 40004U};
    seismic_frame_t frames[5] = {0};
    fill_write_contract_frames(frames, sample_indices, 5U);

    remove_layout();
    make_layout();
    for (size_t format = 0U; format < sizeof(format_versions) / sizeof(format_versions[0]); ++format) {
        const uint16_t version = format_versions[format];
        char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
        FILE *samples = NULL;
        assert(recorder_test_create_format_session(version, session, &samples) == ESP_OK);
        recorder_test_reset_io_trace();

        size_t frames_written = 0U;
        assert(test_fs_write_frames_with_source_indices(
            samples,
            frames,
            sample_indices,
            5U,
            &frames_written
        ) == ESP_OK);
        assert(frames_written == 5U);
        assert(recorder_test_fflush_calls == 0U && recorder_test_fsync_calls == 0U);

        /* Periodic checkpoint maps to fflush+fsync; STOP maps to fflush, sync, close. */
        assert(seismic_recorder_fs_flush(samples, true) == ESP_OK);
        assert(recorder_test_fflush_calls == 1U && recorder_test_fsync_calls == 1U);
        assert(seismic_recorder_fs_flush(samples, false) == ESP_OK);
        assert(recorder_test_fflush_calls == 2U && recorder_test_fsync_calls == 1U);
        assert(seismic_recorder_fs_sync(samples) == ESP_OK);
        assert(recorder_test_fsync_calls == 2U);
        assert(seismic_recorder_fs_close(samples) == ESP_OK);
        assert(recorder_test_fclose_calls == 1U);
        assert_session_decodes_records(session, version, frames, sample_indices, 5U);
    }
    remove_layout();
}


static void test_flush_sync_and_close_failures_keep_decodable_data(void)
{
    static const uint16_t format_versions[] = {
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    const uint64_t sample_indices[5] = {50000U, 50001U, 50002U, 50003U, 50004U};
    seismic_frame_t frames[5] = {0};
    fill_write_contract_frames(frames, sample_indices, 5U);

    for (size_t format = 0U; format < sizeof(format_versions) / sizeof(format_versions[0]); ++format) {
        const uint16_t version = format_versions[format];
        for (size_t failure = 0U; failure < 3U; ++failure) {
            remove_layout();
            make_layout();
            char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
            FILE *samples = NULL;
            assert(recorder_test_create_format_session(version, session, &samples) == ESP_OK);
            recorder_test_reset_io_trace();
            size_t frames_written = 0U;
            assert(test_fs_write_frames_with_source_indices(
                samples,
                frames,
                sample_indices,
                5U,
                &frames_written
            ) == ESP_OK);
            assert(frames_written == 5U);

            if (failure == 0U) {
                injected_fflush_error = true;
                assert(seismic_recorder_fs_flush(samples, false) == ESP_FAIL);
                injected_fflush_error = false;
                assert(recorder_test_fflush_calls == 1U);
            }
            else if (failure == 1U) {
                assert(seismic_recorder_fs_flush(samples, false) == ESP_OK);
                injected_fsync_error = true;
                /* STOP still advances to close after a sync error; no write retry is promised here. */
                assert(seismic_recorder_fs_sync(samples) == ESP_FAIL);
                injected_fsync_error = false;
                assert(recorder_test_fflush_calls == 1U && recorder_test_fsync_calls == 1U);
            }
            else {
                injected_fclose_error = true;
                assert(seismic_recorder_fs_close(samples) == ESP_FAIL);
                injected_fclose_error = false;
                assert(recorder_test_fclose_calls == 1U);
                assert_session_decodes_records(session, version, frames, sample_indices, 5U);
                continue;
            }

            assert(seismic_recorder_fs_close(samples) == ESP_OK);
            assert(recorder_test_fclose_calls == 1U);
            assert_session_decodes_records(session, version, frames, sample_indices, 5U);
        }
    }
    remove_layout();
}


static void test_ad7606_session_writes_v2_blocks_and_source_gaps(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(samples != NULL);
    assert(version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    recorder_test_fwrite_calls = 0U;

    const float scale_uv_per_lsb = 5000000.0f / 32768.0f;
    const int16_t raw_samples[4][SEISMIC_CHANNEL_COUNT] = {
        {-32768, -12345, 0, 32767},
        {-100, 1, 12345, 30000},
        {32767, 0, -1, -32768},
        {123, -456, 789, -1011},
    };
    const uint64_t sample_indices[4] = {100U, 101U, 104U, 105U};
    seismic_frame_t frames[4] = {0};
    for (size_t frame = 0U; frame < 4U; ++frame) {
        frames[frame].timestamp_us = 100000LL + (int64_t)frame * 2000LL;
        if (frame >= 2U) {
            frames[frame].timestamp_us += 4000LL;
        }
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            frames[frame].channel_uv[channel] =
                (float)raw_samples[frame][channel] * scale_uv_per_lsb;
        }
    }

    size_t frames_written = 0U;
    assert(test_fs_write_frames_with_source_indices(
        samples,
        frames,
        sample_indices,
        4U,
        &frames_written
    ) == ESP_OK);
    assert(frames_written == 4U);
    assert(recorder_test_fwrite_calls == 1U);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char path[256];
    const int path_size = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
        session
    );
    assert(path_size > 0 && (size_t)path_size < sizeof(path));
    samples = fopen(path, "rb");
    assert(samples != NULL);

    seismic_recorder_binary_header_t header;
    seismic_recorder_binary_v2_metadata_t metadata;
    assert(fread(&header, sizeof(header), 1U, samples) == 1U);
    assert(header.version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(header.frame_size_bytes == SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES);
    assert(fread(&metadata, sizeof(metadata), 1U, samples) == 1U);
    assert(fabsf(metadata.scale_uv_per_lsb[0] - scale_uv_per_lsb) < 0.0001f);
    assert(metadata.input_range_v == 5.0f);
    assert(metadata.oversampling_ratio == 4U);
    assert(metadata.spi_clock_hz == 10000000U);

    seismic_recorder_binary_v2_block_header_t first_block;
    seismic_recorder_binary_v2_block_header_t second_block;
    assert(fread(&first_block, sizeof(first_block), 1U, samples) == 1U);
    assert(first_block.first_sample_index == 100U);
    assert(first_block.first_timestamp_us == 100000U);
    assert(first_block.frame_count == 2U);
    assert(first_block.flags == 0U);
    uint8_t first_payload[2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    assert(fread(first_payload, sizeof(first_payload), 1U, samples) == 1U);
    assert(fread(&second_block, sizeof(second_block), 1U, samples) == 1U);
    assert(second_block.first_sample_index == 104U);
    assert(second_block.first_timestamp_us == 108000U);
    assert(second_block.frame_count == 2U);
    assert(second_block.flags == SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const size_t offset =
                frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * 2U;
            assert(test_read_i16_le(&first_payload[offset]) == raw_samples[frame][channel]);
        }
    }
    uint8_t second_payload[2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    assert(fread(second_payload, sizeof(second_payload), 1U, samples) == 1U);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const size_t offset =
                frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * 2U;
            assert(test_read_i16_le(&second_payload[offset]) == raw_samples[frame + 2U][channel]);
        }
    }

    assert(fseek(samples, 0L, SEEK_END) == 0);
    const long file_size = ftell(samples);
    assert(file_size > 0L);
    seismic_recorder_session_summary_t summary = {0};
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.format_version = SEISMIC_RECORDER_BINARY_VERSION_V2;
    summary.samples_written = 4U;
    summary.complete = true;
    summary.recovered = true;
    seismic_recorder_samples_reader_t reader;
    assert(seismic_recorder_samples_reader_open(
        test_file_read_at,
        samples,
        (uint64_t)file_size,
        &summary,
        &reader
    ) == ESP_OK);
    seismic_frame_t decoded[4] = {0};
    size_t decoded_count = 0U;
    uint64_t first_index = 0U;
    bool gap_before = true;
    assert(seismic_recorder_samples_reader_next(
        test_file_read_at, samples, &reader, decoded, 4U,
        &decoded_count, &first_index, &gap_before
    ) == ESP_OK);
    assert(decoded_count == 2U && first_index == 100U && !gap_before);
    assert(decoded[0].timestamp_us == 100000LL);
    assert(decoded[1].timestamp_us == 102000LL);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(fabsf(
                decoded[frame].channel_uv[channel] - frames[frame].channel_uv[channel]
            ) <= scale_uv_per_lsb * 0.5f);
        }
    }
    assert(seismic_recorder_samples_reader_next(
        test_file_read_at, samples, &reader, decoded, 4U,
        &decoded_count, &first_index, &gap_before
    ) == ESP_OK);
    assert(decoded_count == 2U && first_index == 104U && gap_before);
    assert(decoded[0].timestamp_us == 108000LL);
    assert(decoded[1].timestamp_us == 110000LL);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(fabsf(
                decoded[frame].channel_uv[channel] - frames[frame + 2U].channel_uv[channel]
            ) <= scale_uv_per_lsb * 0.5f);
        }
    }
    assert(reader.has_gaps && reader.missing_sample_count == 2U);
    assert(seismic_recorder_samples_reader_finish(test_file_read_at, samples, &reader) == ESP_OK);

    assert(fclose(samples) == 0);
    remove_layout();
}


static void test_v1_and_v2_sessions_share_one_object(void)
{
    remove_layout();
    make_layout();

    char first_session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    assert(test_fs_create_session(
        "OBJ_0001", 500U, SEISMIC_CHANNEL_COUNT,
        first_session, &samples
    ) == ESP_OK);
    seismic_frame_t frame = {
        .timestamp_us = 2000LL,
        .channel_uv = {1.0f, 2.0f, 3.0f, 4.0f},
    };
    size_t frames_written = 0U;
    assert(test_fs_write_frames(samples, &frame, 1U, &frames_written) == ESP_OK);
    assert(frames_written == 1U);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char second_session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    uint16_t format_version = 0U;
    assert(test_fs_create_ad7606_session(
        second_session, &samples, &format_version
    ) == ESP_OK);
    assert(format_version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char path[256];
    const char *const session_ids[] = {first_session, second_session};
    const uint16_t expected_versions[] = {
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    for (size_t index = 0U; index < 2U; ++index) {
        const int path_size = snprintf(
            path,
            sizeof(path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
            session_ids[index]
        );
        assert(path_size > 0 && (size_t)path_size < sizeof(path));
        samples = fopen(path, "rb");
        assert(samples != NULL);
        seismic_recorder_binary_header_t header;
        assert(fread(&header, sizeof(header), 1U, samples) == 1U);
        assert(header.version == expected_versions[index]);
        assert(fclose(samples) == 0);
    }
    remove_layout();
}


static void test_large_frame_write_is_chunked_through_staging(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    recorder_test_fwrite_calls = 0U;

    enum { frame_count = SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 1U };
    seismic_frame_t frames[frame_count] = {0};
    for (size_t index = 0U; index < frame_count; ++index) {
        frames[index].timestamp_us = (int64_t)index + 1LL;
        frames[index].channel_uv[0] = (float)index;
    }

    size_t frames_written = 0U;
    assert(seismic_recorder_fs_write_frames(file, frames, frame_count, &frames_written) == ESP_OK);
    assert(frames_written == frame_count);
    /* Each bounded fwrite chunk releases the TFT/SD arbitration window. */
    assert(storage_begin_calls == 3U);
    assert(storage_end_calls == 3U);
    assert(recorder_test_fwrite_calls == 2U);
    assert(seismic_recorder_fs_close(file) == ESP_OK);

    file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin", "rb");
    assert(file != NULL);
    assert(fseek(file, (long)sizeof(seismic_recorder_binary_header_t), SEEK_SET) == 0);
    seismic_frame_t last = {0};
    assert(fseek(file, (long)(sizeof(seismic_frame_t) * (frame_count - 1U)), SEEK_CUR) == 0);
    assert(fread(&last, sizeof(last), 1U, file) == 1U);
    assert(last.timestamp_us == (int64_t)frame_count);
    assert(last.channel_uv[0] == (float)(frame_count - 1U));
    assert(fclose(file) == 0);
    remove_layout();
}

static void test_session_error_cleanup(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_ERR_INVALID_STATE;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = "stale";
    FILE *file = (FILE *)1;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_ERR_INVALID_STATE);
    assert(session[0] == '\0');
    assert(file == NULL);
    assert(storage_begin_calls == 1U);
    assert(storage_end_calls == 0U);

    storage_begin_error = ESP_OK;
    fail_create_directory = true;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_FAIL);
    assert(session[0] != '\0');
    assert(file == NULL);
    assert(storage_begin_calls == 2U);
    assert(storage_end_calls == 1U);
    assert(exclusive_directory_calls == 1U);
    assert(remove_tree_calls == 0U);

    fail_create_directory = false;
    remove_layout();
}

static void test_session_cleanup_after_post_mkdir_failure(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';
    fake_remove_tree_error = ESP_ERR_INVALID_STATE;
    injected_short_write_count = SIZE_MAX;
    injected_fflush_error = true;
    injected_fclose_error = false;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(
        seismic_recorder_fs_create_session(
            "OBJ_0001",
            500,
            4,
            session,
            &file
        ) == ESP_FAIL
    );
    assert(strcmp(session, "S000001") == 0);
    assert(file == NULL);
    assert(exclusive_directory_calls == 1U);
    assert(remove_tree_calls == 1U);
    assert(
        strcmp(
            removed_tree_path,
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001"
        ) == 0
    );
    assert(storage_begin_calls == 1U);
    assert(storage_end_calls == 1U);

    fake_remove_tree_error = ESP_OK;
    injected_fflush_error = false;
    remove_layout();
}

static void test_writer_errors_are_propagated(void)
{
    remove_layout();
    make_layout();
    storage_begin_error = ESP_OK;
    injected_short_write_count = SIZE_MAX;
    injected_fflush_error = false;
    injected_fsync_error = false;
    injected_fclose_error = false;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    seismic_frame_t frames[3] = {0};
    size_t frames_written = 99U;

    injected_short_write_count = 1U;
    assert(seismic_recorder_fs_write_frames(file, frames, 3U, &frames_written) == ESP_FAIL);
    assert(frames_written == 1U);
    injected_short_write_count = SIZE_MAX;

    injected_fflush_error = true;
    assert(seismic_recorder_fs_flush(file, false) == ESP_FAIL);
    injected_fflush_error = false;

    injected_fsync_error = true;
    assert(seismic_recorder_fs_sync(file) == ESP_FAIL);
    injected_fsync_error = false;

    injected_fclose_error = true;
    assert(seismic_recorder_fs_close(file) == ESP_FAIL);
    injected_fclose_error = false;
    remove_layout();
}

static void test_session_id_increments(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007") == 0);
    FILE *marker = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007/marker", "wb");
    assert(marker != NULL);
    assert(fclose(marker) == 0);

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000008") == 0);
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007/marker");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007");
    remove_layout();
}


static void test_staged_artifacts_attach_once_to_the_created_session(void)
{
    remove_layout();
    make_layout();
    write_staged_artifacts();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(file != NULL);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char staged_path[256], session_path[256];
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
            staged_artifact_names[i]
        );
        assert(!path_exists(staged_path));
        assert_file_contents(session_path, staged_artifact_contents[i]);
    }
    assert(seismic_recorder_fs_close(file) == ESP_OK);

    memset(session, 0, sizeof(session));
    file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000002") == 0);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char session_path[256], staged_path[256];
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000002/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        assert(!path_exists(session_path));
        assert(!path_exists(staged_path));
    }
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    remove_layout();
}


static void test_missing_staging_is_a_noop(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(!path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging"));
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    remove_layout();
}


static void test_one_staged_move_failure_does_not_fail_recording(void)
{
    remove_layout();
    make_layout();
    write_staged_artifacts();
    (void)snprintf(
        injected_rename_source,
        sizeof(injected_rename_source),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/preview-v1.bin"
    );

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(file != NULL);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char staged_path[256], session_path[256];
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
            staged_artifact_names[i]
        );
        if (strcmp(staged_artifact_names[i], "preview-v1.bin") == 0) {
            assert(path_exists(staged_path));
            assert(!path_exists(session_path));
            assert_file_contents(staged_path, staged_artifact_contents[i]);
        }
        else {
            assert(!path_exists(staged_path));
            assert_file_contents(session_path, staged_artifact_contents[i]);
        }
    }
    assert(injected_rename_source[0] == '\0');
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    remove_layout();
}


static void test_failed_start_restores_staged_artifacts(void)
{
    remove_layout();
    make_layout();
    write_staged_artifacts();
    injected_fflush_error = true;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_FAIL);
    injected_fflush_error = false;
    assert(strcmp(session, "S000001") == 0);
    assert(file == NULL);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char staged_path[256], session_path[256];
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
            staged_artifact_names[i]
        );
        assert_file_contents(staged_path, staged_artifact_contents[i]);
        assert(!path_exists(session_path));
    }
    remove_layout();
}


static void test_create_session_aborts_on_session_scan_error(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);

    const char *samples_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin";
    FILE *sentinel = fopen(samples_path, "wb");
    assert(sentinel != NULL);
    assert(fputs("preserved", sentinel) >= 0);
    assert(fclose(sentinel) == 0);

    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    reset_directory_fault();
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = "stale";
    FILE *file = (FILE *)1;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_FAIL);
    assert(session[0] == '\0');
    assert(file == NULL);
    assert(exclusive_directory_calls == 0U);
    assert(remove_tree_calls == 0U);
    assert(storage_begin_calls == storage_end_calls);

    sentinel = fopen(samples_path, "rb");
    assert(sentinel != NULL);
    char contents[16] = {0};
    assert(fread(contents, 1U, sizeof(contents) - 1U, sentinel) == 9U);
    assert(strcmp(contents, "preserved") == 0);
    assert(fclose(sentinel) == 0);

    reset_directory_fault();
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;
    memset(session, 0, sizeof(session));
    file = (FILE *)1;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_FAIL);
    assert(session[0] == '\0');
    assert(file == NULL);
    assert(exclusive_directory_calls == 0U);
    assert(remove_tree_calls == 0U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    remove_layout();
}


static void test_listing_missing_and_malformed_metadata_is_safe(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003") == 0);
    /* A gap must not cause presumed slot probes; only directory entries count. */
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007") == 0);

    seismic_recorder_session_summary_t sessions[4] = {0};
    size_t count = 0U;
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fake_summary_error = ESP_ERR_NOT_FOUND;
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 0U);
    assert(fake_known_present_summary_read_calls == 0U);
    assert(strcmp(sessions[0].session_id, "S000007") == 0);
    assert(strcmp(sessions[1].session_id, "S000003") == 0);
    assert(strcmp(sessions[2].session_id, "S000001") == 0);
    for (size_t index = 0U; index < count; ++index) {
        assert(!sessions[index].complete);
        assert(sessions[index].source_name[0] == '\0');
    }

    const char *session_ids[] = {"S000001", "S000003", "S000007"};
    for (size_t index = 0U; index < sizeof(session_ids) / sizeof(session_ids[0]); ++index) {
        char metadata_path[256];
        (void)snprintf(
            metadata_path,
            sizeof(metadata_path),
            "%s/OBJ_0001/sessions/%s/session.json",
            STORAGE_MANAGER_OBJECTS_PATH,
            session_ids[index]
        );
        FILE *metadata = fopen(metadata_path, "wb");
        assert(metadata != NULL);
        assert(fclose(metadata) == 0);
    }

    fake_summary_error = ESP_ERR_INVALID_RESPONSE;
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 0U);
    assert(fake_known_present_summary_read_calls == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(!sessions[index].complete);
    }

    fake_summary_error = ESP_OK;
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 0U);
    assert(fake_known_present_summary_read_calls == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(sessions[index].complete);
        assert(strcmp(sessions[index].source_name, "AD7606") == 0);
    }

    /* A backup-only session must retain the normal recovery path. */
    assert(
        remove(
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003/session.json"
        ) == 0
    );
    FILE *backup = fopen(
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003/session.json.bak",
        "wb"
    );
    assert(backup != NULL);
    assert(fclose(backup) == 0);
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 1U);
    assert(fake_known_present_summary_read_calls == 2U);

    assert(storage_begin_calls == storage_end_calls);
    fake_summary_error = ESP_FAIL;
    remove_layout();
}


static void test_listing_propagates_directory_errors(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);

    seismic_recorder_session_summary_t sessions[2] = {0};
    size_t count = 0U;
    storage_begin_error = ESP_OK;
    fake_summary_error = ESP_OK;

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EACCES;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_opendir_calls == 1U);
    assert(directory_closedir_calls == 0U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR;
    directory_fault_call = 2U;
    directory_fault_errno = EACCES;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_opendir_calls == 2U);
    assert(directory_closedir_calls == 1U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_readdir_calls == 1U);
    assert(directory_closedir_calls == 1U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR;
    directory_fault_call = 2U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_readdir_calls == 2U);
    /* The injected second read can hit the parent census before a child
     * directory is opened (for example when '.'/'..' are returned first).
     * The invariant is leak freedom, not a platform-specific close count. */
    assert(directory_closedir_calls == directory_opendir_calls);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_closedir_calls == 2U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR;
    directory_fault_call = 2U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 1U);
    assert(!sessions[0].complete);
    assert(directory_closedir_calls == 2U);
    assert(storage_begin_calls == storage_end_calls);

    fake_summary_error = ESP_FAIL;
    reset_directory_fault();
    remove_layout();
}

int main(void)
{
#if defined(MAX371_RECORDER_RAW_ORACLE_HOST_TEST)
    test_exhaustive_ad7606_codes_round_trip_v2();
    test_nonzero_offset_is_not_raw_reversible();
    test_max371_v2_file_staging_boundaries_and_stop_tail();
#endif
    test_invalid_arguments();
    test_session_id_parser_is_canonical();
    test_session_header_and_frames();
    test_ad7606_session_writes_v2_blocks_and_source_gaps();
    test_v1_and_v2_sessions_share_one_object();
    test_large_frame_write_is_chunked_through_staging();
    test_recorder_batch_and_staging_write_budget();
    test_write_budget_rejects_per_frame_debatching();
    test_short_write_positions_preserve_decodable_records();
    test_writer_flush_sync_and_close_call_cadence();
    test_flush_sync_and_close_failures_keep_decodable_data();
    test_session_error_cleanup();
    test_session_cleanup_after_post_mkdir_failure();
    test_writer_errors_are_propagated();
    test_session_id_increments();
    test_staged_artifacts_attach_once_to_the_created_session();
    test_missing_staging_is_a_noop();
    test_one_staged_move_failure_does_not_fail_recording();
    test_failed_start_restores_staged_artifacts();
    test_create_session_aborts_on_session_scan_error();
    test_listing_missing_and_malformed_metadata_is_safe();
    test_listing_propagates_directory_errors();
#if defined(SEISMIC_RECORDER_GAP_PIPELINE_TEST)
    test_acquisition_gap_survives_ring_cache_v2_reader();
    test_ring_overflow_survives_as_recorded_gap();
#endif
    test_session_range_v1_preserves_timestamps_and_bounds();
    test_session_range_v2_seeks_blocks_and_preserves_gaps();
    return 0;
}
