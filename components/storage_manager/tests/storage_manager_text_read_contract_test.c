#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "storage_manager.h"
#include "storage_manager_internal.h"


#define STORAGE_TEXT_TEST_PATH "storage_manager_text_read_contract_tmp.txt"
#define STORAGE_TEXT_MISSING_PATH "storage_manager_text_missing_contract_tmp.txt"


static esp_err_t fake_operation_begin_result;
static unsigned fake_operation_begin_calls;
static unsigned fake_operation_end_calls;
static unsigned fake_stat_calls;
static int fake_fopen_errno;
static bool fake_probe_io_error;
static bool fake_stream_error;


esp_err_t storage_manager_operation_begin(void)
{
    ++fake_operation_begin_calls;
    return fake_operation_begin_result;
}


void storage_manager_operation_end(void)
{
    ++fake_operation_end_calls;
}


static int storage_manager_test_stat(
    const char *path,
    struct stat *info
)
{
    ++fake_stat_calls;
    return stat(path, info);
}


static FILE *storage_manager_test_fopen(
    const char *path,
    const char *mode
)
{
    if (fake_fopen_errno != 0) {
        errno = fake_fopen_errno;
        return NULL;
    }

    return fopen(path, mode);
}


static int storage_manager_test_fgetc(
    FILE *file
)
{
    if (fake_probe_io_error) {
        fake_stream_error = true;
        errno = EIO;
        return EOF;
    }

    return fgetc(file);
}


static int storage_manager_test_ferror(
    FILE *file
)
{
    return fake_stream_error ? 1 : ferror(file);
}


/* Rename helper-local symbols so both production translation units can share this TU. */
#define TAG storage_manager_text_helper_TAG
#define storage_text_log_io storage_manager_text_helper_log_io
#define storage_text_errno_to_error storage_manager_text_helper_errno_to_error
#define stat(...) storage_manager_test_stat(__VA_ARGS__)
#include "../storage_manager_text_helpers.c"
#undef TAG
#undef storage_text_log_io
#undef storage_text_errno_to_error
#undef stat

#define fopen(...) storage_manager_test_fopen(__VA_ARGS__)
#define fgetc(...) storage_manager_test_fgetc(__VA_ARGS__)
#define ferror(...) storage_manager_test_ferror(__VA_ARGS__)
#include "../storage_manager_text_read.c"
#undef fopen
#undef fgetc
#undef ferror


static void reset_fake_state(void)
{
    fake_operation_begin_result = ESP_OK;
    fake_operation_begin_calls = 0U;
    fake_operation_end_calls = 0U;
    fake_stat_calls = 0U;
    fake_fopen_errno = 0;
    fake_probe_io_error = false;
    fake_stream_error = false;
    storage_manager_test_last_log_level = '\0';
    storage_manager_test_last_log_message[0] = '\0';
}


static void write_text_file(
    const char *path,
    const char *text
)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}


static void remove_test_files(void)
{
    (void)remove(STORAGE_TEXT_TEST_PATH);
    (void)remove(STORAGE_TEXT_TEST_PATH ".bak");
}


static void test_known_present_reads_without_recovery_probe(void)
{
    remove_test_files();
    write_text_file(STORAGE_TEXT_TEST_PATH, "primary");
    reset_fake_state();

    char buffer[32] = {0};
    assert(
        storage_manager_read_text_known_present(
            STORAGE_TEXT_TEST_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_OK
    );
    assert(strcmp(buffer, "primary") == 0);
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 1U);
    assert(fake_stat_calls == 0U);

    reset_fake_state();
    memset(buffer, 0, sizeof(buffer));
    assert(
        storage_manager_read_text(
            STORAGE_TEXT_TEST_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_OK
    );
    assert(strcmp(buffer, "primary") == 0);
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 1U);
    assert(fake_stat_calls == 1U);

    remove_test_files();
}


static void test_normal_reader_keeps_backup_recovery(void)
{
    remove_test_files();
    write_text_file(STORAGE_TEXT_TEST_PATH ".bak", "backup");
    reset_fake_state();

    char buffer[32] = {0};
    assert(
        storage_manager_read_text(
            STORAGE_TEXT_TEST_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_OK
    );
    assert(strcmp(buffer, "backup") == 0);
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 1U);
    assert(fake_stat_calls == 2U);

    struct stat info;
    assert(stat(STORAGE_TEXT_TEST_PATH, &info) == 0);
    assert(stat(STORAGE_TEXT_TEST_PATH ".bak", &info) != 0);
    remove_test_files();
}


static void test_known_present_errors_still_pair_operation_guard(void)
{
    reset_fake_state();
    fake_operation_begin_result = ESP_ERR_INVALID_STATE;

    char buffer[32] = {0};
    assert(
        storage_manager_read_text_known_present(
            STORAGE_TEXT_TEST_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_ERR_INVALID_STATE
    );
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 0U);
    assert(fake_stat_calls == 0U);

    reset_fake_state();
    assert(
        storage_manager_read_text_known_present(
            STORAGE_TEXT_TEST_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_ERR_NOT_FOUND
    );
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 1U);
    assert(fake_stat_calls == 0U);
}


static void test_missing_optional_file_logs_at_debug(void)
{
    (void)remove(STORAGE_TEXT_MISSING_PATH);
    reset_fake_state();

    char buffer[32] = {0};
    assert(
        storage_manager_read_text(
            STORAGE_TEXT_MISSING_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_ERR_NOT_FOUND
    );
    assert(fake_operation_end_calls == 1U);
    assert(storage_manager_test_last_log_level == 'D');
    assert(
        strcmp(
            storage_manager_test_last_log_message,
            "Could not open " STORAGE_TEXT_MISSING_PATH ": errno=2"
        ) == 0
    );
}


static void test_non_missing_open_error_remains_error(void)
{
    write_text_file(STORAGE_TEXT_TEST_PATH, "primary");
    reset_fake_state();
    fake_fopen_errno = EACCES;

    char buffer[32] = {0};
    assert(
        storage_manager_read_text_known_present(
            STORAGE_TEXT_TEST_PATH,
            buffer,
            sizeof(buffer)
        ) == ESP_FAIL
    );
    assert(fake_operation_end_calls == 1U);
    assert(storage_manager_test_last_log_level == 'E');

    char expected[128];
    (void)snprintf(
        expected,
        sizeof(expected),
        "Could not open %s: errno=%d",
        STORAGE_TEXT_TEST_PATH,
        EACCES
    );
    assert(strcmp(storage_manager_test_last_log_message, expected) == 0);
    remove_test_files();
}


static void test_buffer_size_boundaries(void)
{
    remove_test_files();
    write_text_file(STORAGE_TEXT_TEST_PATH, "1234");
    reset_fake_state();

    char buffer[5] = {0};
    const esp_err_t exact_size_err = storage_manager_read_text_known_present(
        STORAGE_TEXT_TEST_PATH,
        buffer,
        sizeof(buffer)
    );
    assert(exact_size_err == ESP_OK);
    assert(strcmp(buffer, "1234") == 0);
    assert(fake_operation_end_calls == 1U);

    remove_test_files();
    write_text_file(STORAGE_TEXT_TEST_PATH, "12345");
    reset_fake_state();
    memset(buffer, 0, sizeof(buffer));

    const esp_err_t oversized_err = storage_manager_read_text_known_present(
        STORAGE_TEXT_TEST_PATH,
        buffer,
        sizeof(buffer)
    );
    assert(oversized_err == ESP_ERR_INVALID_SIZE);
    assert(fake_operation_end_calls == 1U);
    remove_test_files();
}


static void test_size_probe_io_error_is_reported(void)
{
    remove_test_files();
    write_text_file(STORAGE_TEXT_TEST_PATH, "1234");
    reset_fake_state();
    fake_probe_io_error = true;

    char buffer[5] = {0};
    const esp_err_t err = storage_manager_read_text_known_present(
        STORAGE_TEXT_TEST_PATH,
        buffer,
        sizeof(buffer)
    );
    char expected_log[128];
    (void)snprintf(
        expected_log,
        sizeof(expected_log),
        "Read failed for %s: errno=%d",
        STORAGE_TEXT_TEST_PATH,
        EIO
    );
    const bool logged_probe_errno =
        strcmp(storage_manager_test_last_log_message, expected_log) == 0;
    const unsigned operation_end_calls = fake_operation_end_calls;
    const bool probe_error_was_injected = fake_stream_error;
    remove_test_files();

    assert(probe_error_was_injected);
    assert(err == ESP_FAIL);
    assert(operation_end_calls == 1U);
    assert(logged_probe_errno);
}


static void test_invalid_arguments(void)
{
    char buffer[4] = {0};
    assert(storage_manager_read_text_known_present(NULL, buffer, sizeof(buffer)) == ESP_ERR_INVALID_ARG);
    assert(storage_manager_read_text_known_present("path", NULL, sizeof(buffer)) == ESP_ERR_INVALID_ARG);
    assert(storage_manager_read_text_known_present("path", buffer, 1U) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_invalid_arguments();
    test_buffer_size_boundaries();
    test_known_present_reads_without_recovery_probe();
    test_normal_reader_keeps_backup_recovery();
    test_known_present_errors_still_pair_operation_guard();
    test_missing_optional_file_logs_at_debug();
    test_non_missing_open_error_remains_error();
    test_size_probe_io_error_is_reported();
    puts("storage manager text read contract tests passed");
    return 0;
}
