#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "storage_manager.h"
#include "storage_manager_internal.h"


#define STORAGE_TEXT_TEST_PATH "storage_manager_text_read_contract_tmp.txt"


static esp_err_t fake_operation_begin_result;
static unsigned fake_operation_begin_calls;
static unsigned fake_operation_end_calls;
static unsigned fake_stat_calls;


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

#include "../storage_manager_text_read.c"


static void reset_fake_state(void)
{
    fake_operation_begin_result = ESP_OK;
    fake_operation_begin_calls = 0U;
    fake_operation_end_calls = 0U;
    fake_stat_calls = 0U;
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
    test_known_present_reads_without_recovery_probe();
    test_normal_reader_keeps_backup_recovery();
    test_known_present_errors_still_pair_operation_guard();
    puts("storage manager text read contract tests passed");
    return 0;
}
